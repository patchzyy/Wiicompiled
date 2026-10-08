#include "pvr.h"
#include "prs.h"
#include "../core/common.h"
#include <filesystem>
#include <fstream>

namespace sonic {

namespace {

enum PixelFormat { PF_ARGB1555 = 0, PF_RGB565 = 1, PF_ARGB4444 = 2, PF_YUV422 = 3, PF_BUMP = 4, PF_RGB555 = 5, PF_ARGB8888 = 6 };

// Morton-style twiddle: y occupies the even bits, x the odd bits.
struct TwiddleTable {
    u32 t[1024];
    TwiddleTable() {
        for (u32 i = 0; i < 1024; i++) {
            u32 v = 0;
            for (u32 b = 0; b < 10; b++)
                if (i & (1u << b)) v |= 1u << (2 * b);
            t[i] = v;
        }
    }
};
const TwiddleTable g_tw;

inline u32 twiddle(u32 x, u32 y) { return (g_tw.t[x] << 1) | g_tw.t[y]; }

inline u32 expand(u32 v, int bits) {
    // expand an n-bit channel to 8 bits
    switch (bits) {
        case 1: return v ? 255 : 0;
        case 4: return (v << 4) | v;
        case 5: return (v << 3) | (v >> 2);
        case 6: return (v << 2) | (v >> 4);
        default: return v;
    }
}

inline u32 decode16(u16 c, int pf) {
    u32 a = 255, r, g, b;
    switch (pf) {
        case PF_ARGB1555:
            a = expand(c >> 15, 1); r = expand((c >> 10) & 31, 5); g = expand((c >> 5) & 31, 5); b = expand(c & 31, 5);
            break;
        case PF_RGB565:
            r = expand(c >> 11, 5); g = expand((c >> 5) & 63, 6); b = expand(c & 31, 5);
            break;
        case PF_ARGB4444:
            a = expand(c >> 12, 4); r = expand((c >> 8) & 15, 4); g = expand((c >> 4) & 15, 4); b = expand(c & 15, 4);
            break;
        case PF_RGB555:
            r = expand((c >> 10) & 31, 5); g = expand((c >> 5) & 31, 5); b = expand(c & 31, 5);
            break;
        default:
            r = g = b = (c >> 8);
            break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);  // RGBA in memory order
}

inline u8 clamp255(int v) { return u8(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void yuvPair(u16 c0, u16 c1, u32& o0, u32& o1) {
    int y0 = c0 >> 8, u = c0 & 0xFF, y1 = c1 >> 8, v = c1 & 0xFF;
    auto conv = [&](int y) {
        int r = int(y + 1.375f * (v - 128));
        int g = int(y - 0.6875f * (v - 128) - 0.34375f * (u - 128));
        int b = int(y + 1.71875f * (u - 128));
        return u32(clamp255(r)) | (u32(clamp255(g)) << 8) | (u32(clamp255(b)) << 16) | 0xFF000000u;
    };
    o0 = conv(y0);
    o1 = conv(y1);
}

// Size in bytes of one mip level for a given data format
size_t levelBytes(int w, int h, int bpp) { return size_t(w) * h * bpp / 8; }

}  // namespace

bool decodePvr(const u8* data, size_t len, Image& out, const std::vector<u32>* palette) {
    size_t p = 0;
    // skip GBIX/GCIX
    while (p + 8 <= len && (memcmp(data + p, "GBIX", 4) == 0 || memcmp(data + p, "GCIX", 4) == 0)) {
        p += 8 + rd<u32>(data + p + 4);
    }
    if (p + 16 > len || memcmp(data + p, "PVRT", 4) != 0) return false;
    u32 chunkLen = rd<u32>(data + p + 4);
    int pf = data[p + 8];
    int df = data[p + 9];
    int w = rd<u16>(data + p + 12);
    int h = rd<u16>(data + p + 14);
    const u8* px = data + p + 16;
    size_t avail = std::min<size_t>(len - (p + 16), chunkLen >= 8 ? chunkLen - 8 : 0);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    out.width = w;
    out.height = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    u32* dst = (u32*)out.rgba.data();

    auto texel16 = [&](size_t idx) -> u16 { return rd<u16>(px + idx * 2); };

    bool isPal4 = df == 5 || df == 6;
    bool isPal8 = df == 7 || df == 8;
    bool isVQ = df == 3 || df == 4 || df == 0x10 || df == 0x11;
    bool mips = df == 2 || df == 4 || df == 6 || df == 8 || df == 0x11 || df == 0x12;
    int bpp = isPal4 ? 4 : isPal8 ? 8 : (pf == PF_ARGB8888 && !isVQ ? 32 : 16);

    if (isVQ) {
        int codes = 256;
        if (df == 0x10) codes = w <= 16 ? 16 : w == 32 ? 32 : w == 64 ? 128 : 256;
        if (df == 0x11) codes = w <= 16 ? 16 : w == 32 ? 64 : 256;
        size_t cbBytes = size_t(codes) * 8;
        size_t idxBytes = size_t(w / 2) * (h / 2);
        if (avail < cbBytes + idxBytes) return false;
        const u8* cb = px;
        // full-size level is the last block of indices
        const u8* idx = px + avail - idxBytes;
        if (!mips) idx = px + cbBytes;
        int hw = w / 2, hh = h / 2;
        int mn = std::min(hw, hh);
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < hw; x++) {
                u32 ti;
                if (hw == hh) ti = twiddle(x, y);
                else if (hw > hh) ti = u32((x / mn) * mn * mn) + twiddle(x % mn, y);
                else ti = u32((y / mn) * mn * mn) + twiddle(x, y % mn);
                u8 code = idx[ti];
                const u8* c = cb + code * 8;
                for (int k = 0; k < 4; k++) {
                    int dx = k >> 1, dy = k & 1;
                    u16 v = rd<u16>(c + k * 2);
                    dst[(y * 2 + dy) * w + (x * 2 + dx)] = decode16(v, pf);
                }
            }
        if (pf == PF_YUV422) { /* rare: VQ+YUV not used by SADX */ }
    } else if (isPal4 || isPal8) {
        size_t lvl = levelBytes(w, h, bpp);
        if (avail < lvl) return false;
        const u8* base = mips ? px + avail - lvl : px;
        int mn = std::min(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                u32 ti = (w == h) ? twiddle(x, y) : (w > h ? u32((x / mn) * mn * mn) + twiddle(x % mn, y) : u32((y / mn) * mn * mn) + twiddle(x, y % mn));
                u32 ci = isPal4 ? ((base[ti >> 1] >> ((ti & 1) * 4)) & 15) : base[ti];
                u32 col;
                if (palette && ci < palette->size()) {
                    u32 c = (*palette)[ci];  // ARGB
                    col = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000);
                } else {
                    u32 g = isPal4 ? ci * 17 : ci;
                    col = g | (g << 8) | (g << 16) | 0xFF000000;
                }
                dst[y * w + x] = col;
            }
    } else {
        size_t lvl = levelBytes(w, h, bpp);
        if (avail < lvl) {
            SONIC_LOGD("PVR: truncated data (%zu < %zu)", avail, lvl);
            return false;
        }
        bool twiddled = df == 1 || df == 2 || df == 0x0D || df == 0x12;
        // Twiddled mipmap chains store the smallest level first (full level last).
        // Rectangle/stride data on PC stores the full level first.
        const u8* base = (mips && twiddled) ? px + avail - lvl : px;
        int stride = w;
        if (df == 0x0B) stride = w;  // stride textures: width is the stride on PC
        int mn = std::min(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t ti;
                if (twiddled) {
                    if (w == h) ti = twiddle(x, y);
                    else if (w > h) ti = size_t(x / mn) * mn * mn + twiddle(x % mn, y);
                    else ti = size_t(y / mn) * mn * mn + twiddle(x, y % mn);
                } else {
                    ti = size_t(y) * stride + x;
                }
                if (bpp == 32) {
                    u32 c = rd<u32>(base + ti * 4);  // ARGB8888 little endian => B,G,R,A bytes
                    dst[y * w + x] = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000);
                } else if (pf == PF_YUV422) {
                    // pairs of texels share U/V
                    if ((x & 1) == 0 && x + 1 < w) {
                        size_t ti1;
                        if (twiddled) {
                            if (w == h) ti1 = twiddle(x + 1, y);
                            else if (w > h) ti1 = size_t((x + 1) / mn) * mn * mn + twiddle((x + 1) % mn, y);
                            else ti1 = size_t(y / mn) * mn * mn + twiddle(x + 1, y % mn);
                        } else ti1 = ti + 1;
                        u32 a, b;
                        yuvPair(rd<u16>(base + ti * 2), rd<u16>(base + ti1 * 2), a, b);
                        dst[y * w + x] = a;
                        dst[y * w + x + 1] = b;
                    }
                } else {
                    dst[y * w + x] = decode16(rd<u16>(base + ti * 2), pf);
                }
            }
        (void)texel16;
    }
    out.hasAlpha = false;
    for (size_t i = 0; i < size_t(w) * h; i++)
        if ((dst[i] >> 24) != 0xFF) { out.hasAlpha = true; break; }
    return true;
}

bool PvmArchive::parseHeader(const u8* d, size_t len, bool needData) {
    entries_.clear();
    if (len < 12 || memcmp(d, "PVMH", 4) != 0) return false;
    u32 hsize = rd<u32>(d + 4);
    u16 flags = rd<u16>(d + 8);
    u16 count = rd<u16>(d + 10);
    size_t p = 12;
    for (u16 i = 0; i < count; i++) {
        PvmEntry e;
        if (p + 2 > len) return false;
        p += 2;  // index
        if (flags & 0x8) {
            if (p + 28 > len) return false;
            char nm[29] = {0};
            memcpy(nm, d + p, 28);
            e.name = nm;
            p += 28;
        }
        if (flags & 0x4) p += 2;  // format
        if (flags & 0x2) p += 2;  // dimensions
        if (flags & 0x1) {
            if (p + 4 > len) return false;
            e.gbix = rd<u32>(d + p);
            p += 4;
        }
        entries_.push_back(e);
    }
    if (!needData) return true;
    // Locate texture data
    size_t off = 8 + hsize;
    for (auto& e : entries_) {
        // find the next GBIX or PVRT tag (skip alignment padding)
        size_t q = off;
        while (q + 8 <= len && memcmp(d + q, "GBIX", 4) != 0 && memcmp(d + q, "PVRT", 4) != 0 && memcmp(d + q, "GCIX", 4) != 0) q++;
        if (q + 8 > len) return false;
        size_t start = q;
        while (q + 8 <= len && (memcmp(d + q, "GBIX", 4) == 0 || memcmp(d + q, "GCIX", 4) == 0)) q += 8 + rd<u32>(d + q + 4);
        if (q + 8 > len || memcmp(d + q, "PVRT", 4) != 0) return false;
        size_t end = q + 8 + rd<u32>(d + q + 4);
        if (end > len) end = len;
        e.offset = start;
        e.size = end - start;
        off = end;
    }
    return true;
}

bool PvmArchive::parse(std::vector<u8>&& data) {
    data_ = std::move(data);
    if (data_.size() >= 4 && memcmp(data_.data(), "PVMH", 4) != 0) {
        // maybe PRS compressed
        std::vector<u8> dec;
        if (prsDecompress(data_.data(), data_.size(), dec) && dec.size() >= 4 && memcmp(dec.data(), "PVMH", 4) == 0)
            data_ = std::move(dec);
    }
    return parseHeader(data_.data(), data_.size(), true);
}

bool PvmArchive::open(const std::string& path) {
    path_ = path;
    std::vector<u8> d;
    if (!readFile(path, d)) return false;
    return parse(std::move(d));
}

bool PvmArchive::readHeaderOnly(const std::string& path) {
    path_ = path;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    u8 hdr[8];
    if (!f.read((char*)hdr, 8)) return false;
    if (memcmp(hdr, "PVMH", 4) != 0) {
        // compressed - fall back to full parse
        return open(path);
    }
    u32 hsize = rd<u32>(hdr + 4);
    if (hsize > 1 << 20) return false;
    std::vector<u8> buf(8 + hsize);
    memcpy(buf.data(), hdr, 8);
    f.read((char*)buf.data() + 8, hsize);
    bool ok = parseHeader(buf.data(), buf.size(), false);
    return ok;
}

bool PvmArchive::decode(size_t index, Image& out) const {
    if (index >= entries_.size() || !entries_[index].size) return false;
    return decodePvr(data_.data() + entries_[index].offset, entries_[index].size, out);
}

}  // namespace sonic
