// sonic_formats.cpp - see sonic_formats.h.
#include "sonic/sonic_formats.h"

#include <algorithm>
#include <cstring>

namespace sonic_mkw {

float BeReadF32(const uint8_t* p) {
    const uint32_t bits = BeRead32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void BePush16(Bytes& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void BePush32(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return text;
}

std::vector<std::string> PathTokens(const std::string& path) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : path) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) {
            current.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

static void PadTo(Bytes& out, size_t alignment, uint8_t fill = 0) {
    while (out.size() % alignment) out.push_back(fill);
}

// =====================================================================================
// Yaz0
// =====================================================================================

bool IsYaz0(const uint8_t* data, size_t size) {
    return size >= 16 && std::memcmp(data, "Yaz0", 4) == 0;
}

bool Yaz0Decode(const uint8_t* data, size_t size, Bytes& out) {
    if (!IsYaz0(data, size)) return false;
    const uint32_t outSize = BeRead32(data + 4);
    out.assign(outSize, 0);
    size_t src = 16;
    size_t dst = 0;
    uint32_t code = 0;
    int bits = 0;
    while (dst < outSize) {
        if (bits == 0) {
            if (src >= size) return false;
            code = data[src++];
            bits = 8;
        }
        if (code & 0x80) {
            if (src >= size) return false;
            out[dst++] = data[src++];
        } else {
            if (src + 1 >= size) return false;
            const uint8_t b1 = data[src++];
            const uint8_t b2 = data[src++];
            const size_t dist = (size_t(b1 & 0x0F) << 8 | b2) + 1;
            size_t length = b1 >> 4;
            if (length == 0) {
                if (src >= size) return false;
                length = size_t(data[src++]) + 0x12;
            } else {
                length += 2;
            }
            if (dist > dst) return false;
            for (size_t i = 0; i < length && dst < outSize; ++i, ++dst) {
                out[dst] = out[dst - dist];
            }
        }
        code <<= 1;
        --bits;
    }
    return true;
}

// Greedy LZ77 with hash chains and one step of lazy matching. Not as tight as
// Nintendo's encoder, but files stay close to their original size, which matters
// because some loaders read whole archives into fixed-size heaps.
Bytes Yaz0Encode(const uint8_t* data, size_t size) {
    constexpr size_t kWindow = 0x1000;
    constexpr size_t kMaxLen = 0x111;
    constexpr size_t kMinLen = 3;
    constexpr int kHashBits = 15;
    constexpr int kMaxChain = 96;

    Bytes out;
    out.reserve(size + size / 8 + 32);
    out.insert(out.end(), {'Y', 'a', 'z', '0'});
    BePush32(out, uint32_t(size));
    out.resize(16, 0);

    std::vector<int32_t> head(size_t(1) << kHashBits, -1);
    std::vector<int32_t> prev(size, -1);
    auto hashAt = [&](size_t p) -> uint32_t {
        return ((uint32_t(data[p]) << 16 | uint32_t(data[p + 1]) << 8 | data[p + 2]) * 2654435761u) >>
               (32 - kHashBits);
    };
    size_t inserted = 0;
    auto insertUpTo = [&](size_t end) {
        for (; inserted < end; ++inserted) {
            if (inserted + 2 < size) {
                const uint32_t h = hashAt(inserted);
                prev[inserted] = head[h];
                head[h] = int32_t(inserted);
            }
        }
    };
    auto findMatch = [&](size_t pos, size_t& bestDist) -> size_t {
        if (pos + kMinLen > size) return 0;
        insertUpTo(pos);
        size_t best = 0;
        const size_t maxLen = std::min(kMaxLen, size - pos);
        int32_t cand = head[hashAt(pos)];
        int chain = 0;
        while (cand >= 0 && chain++ < kMaxChain) {
            const size_t c = size_t(cand);
            if (pos - c > kWindow) break;
            if (data[c + best] == data[pos + best]) {
                size_t len = 0;
                while (len < maxLen && data[c + len] == data[pos + len]) ++len;
                if (len > best) {
                    best = len;
                    bestDist = pos - c;
                    if (len == maxLen) break;
                }
            }
            cand = prev[c];
        }
        return best >= kMinLen ? best : 0;
    };

    size_t pos = 0;
    while (pos < size) {
        const size_t codePos = out.size();
        out.push_back(0);
        uint8_t code = 0;
        for (int bit = 0; bit < 8 && pos < size; ++bit) {
            size_t dist = 0;
            size_t len = findMatch(pos, dist);
            if (len) {
                // Lazy step: prefer a literal now if the next byte starts a longer match.
                size_t dist2 = 0;
                const size_t len2 = findMatch(pos + 1, dist2);
                if (len2 > len + 1) len = 0;
            }
            if (!len) {
                code |= uint8_t(0x80 >> bit);
                out.push_back(data[pos]);
                ++pos;
                continue;
            }
            const size_t d = dist - 1;
            if (len < 0x12) {
                out.push_back(uint8_t(((len - 2) << 4) | (d >> 8)));
                out.push_back(uint8_t(d & 0xFF));
            } else {
                out.push_back(uint8_t(d >> 8));
                out.push_back(uint8_t(d & 0xFF));
                out.push_back(uint8_t(len - 0x12));
            }
            pos += len;
        }
        out[codePos] = code;
    }
    return out;
}

// =====================================================================================
// U8
// =====================================================================================

bool U8Archive::Parse(const uint8_t* data, size_t size) {
    nodes_.clear();
    if (size < 0x20 || BeRead32(data) != 0x55AA382Du) return false;
    const uint32_t rootOffset = BeRead32(data + 4);
    if (uint64_t(rootOffset) + 12 > size) return false;
    const uint8_t* root = data + rootOffset;
    const uint32_t count = BeRead32(root + 8);
    if (count == 0 || rootOffset + uint64_t(count) * 12 > size) return false;
    const uint8_t* strings = root + size_t(count) * 12;
    const size_t stringsAvail = size - (rootOffset + size_t(count) * 12);
    nodes_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* n = root + size_t(i) * 12;
        const uint32_t typeName = BeRead32(n);
        const uint32_t a = BeRead32(n + 4);
        const uint32_t b = BeRead32(n + 8);
        Node& node = nodes_[i];
        node.isDir = (typeName >> 24) != 0;
        const uint32_t nameOffset = typeName & 0xFFFFFF;
        if (nameOffset >= stringsAvail) return false;
        const char* name = reinterpret_cast<const char*>(strings + nameOffset);
        node.name.assign(name, strnlen(name, stringsAvail - nameOffset));
        if (node.isDir) {
            node.parent = a;
            node.next = b;
        } else {
            if (uint64_t(a) + b > size) return false;
            node.data.assign(data + a, data + a + b);
        }
    }
    return nodes_[0].isDir;
}

std::string U8Archive::PathOf(size_t index) const {
    // Walk from the root keeping the directory stack.
    std::vector<std::pair<uint32_t, std::string>> stack;  // (end index, name)
    for (size_t i = 1; i < nodes_.size(); ++i) {
        while (!stack.empty() && i >= stack.back().first) stack.pop_back();
        if (i == index) {
            std::string path;
            for (const auto& entry : stack) {
                path += entry.second;
                path += '/';
            }
            return path + nodes_[i].name;
        }
        if (nodes_[i].isDir) stack.emplace_back(nodes_[i].next, nodes_[i].name);
    }
    return {};
}

std::vector<size_t> U8Archive::Files() const {
    std::vector<size_t> files;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].isDir) files.push_back(i);
    }
    return files;
}

Bytes U8Archive::Build() const {
    Bytes strings;
    std::vector<uint32_t> nameOffsets(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
        nameOffsets[i] = uint32_t(strings.size());
        strings.insert(strings.end(), nodes_[i].name.begin(), nodes_[i].name.end());
        strings.push_back(0);
    }
    // Like Nintendo's and mkw-sp's packers, the node+string table size is padded
    // so that it ends exactly where the file data starts.
    const uint32_t dataOffset = (0x20 + uint32_t(nodes_.size() * 12 + strings.size()) + 0x1F) & ~0x1Fu;
    const uint32_t headerSize = dataOffset - 0x20;

    Bytes out(dataOffset, 0);
    BeWrite32(out.data(), 0x55AA382Du);
    BeWrite32(out.data() + 4, 0x20);
    BeWrite32(out.data() + 8, headerSize);
    BeWrite32(out.data() + 12, dataOffset);
    std::memcpy(out.data() + 0x20 + nodes_.size() * 12, strings.data(), strings.size());

    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        uint8_t* n = out.data() + 0x20 + i * 12;
        BeWrite32(n, (node.isDir ? 0x01000000u : 0u) | nameOffsets[i]);
        if (node.isDir) {
            BeWrite32(n + 4, node.parent);
            BeWrite32(n + 8, node.next);
        } else {
            PadTo(out, 0x20);
            n = out.data() + 0x20 + i * 12;  // `out` may have reallocated
            BeWrite32(n + 4, uint32_t(out.size()));
            BeWrite32(n + 8, uint32_t(node.data.size()));
            out.insert(out.end(), node.data.begin(), node.data.end());
        }
    }
    PadTo(out, 0x20);
    return out;
}

// =====================================================================================
// TPL
// =====================================================================================

bool TplReadInfo(const uint8_t* data, size_t size, TplInfo& out) {
    if (size < 0x14 || BeRead32(data) != 0x0020AF30u || BeRead32(data + 4) == 0) return false;
    const uint32_t table = BeRead32(data + 8);
    if (uint64_t(table) + 8 > size) return false;
    const uint32_t header = BeRead32(data + table);
    if (uint64_t(header) + 0x24 > size) return false;
    const uint8_t* h = data + header;
    out.height = BeRead16(h);
    out.width = BeRead16(h + 2);
    out.format = BeRead32(h + 4);
    out.wrapS = BeRead32(h + 12);
    out.wrapT = BeRead32(h + 16);
    return out.width > 0 && out.height > 0;
}

Bytes TplMakeRgba8(const uint8_t* rgba, int width, int height, uint32_t wrapS, uint32_t wrapT) {
    constexpr uint32_t kHeaderOffset = 0x14;
    constexpr uint32_t kDataOffset = 0x40;
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    Bytes out(kDataOffset + size_t(blocksX) * blocksY * 64, 0);
    BeWrite32(out.data(), 0x0020AF30u);
    BeWrite32(out.data() + 4, 1);
    BeWrite32(out.data() + 8, 0x0C);
    BeWrite32(out.data() + 0x0C, kHeaderOffset);
    BeWrite32(out.data() + 0x10, 0);
    uint8_t* h = out.data() + kHeaderOffset;
    BeWrite16(h, uint16_t(height));
    BeWrite16(h + 2, uint16_t(width));
    BeWrite32(h + 4, 6);  // GX_TF_RGBA8
    BeWrite32(h + 8, kDataOffset);
    BeWrite32(h + 12, wrapS);
    BeWrite32(h + 16, wrapT);
    BeWrite32(h + 20, 1);  // min filter: linear
    BeWrite32(h + 24, 1);  // mag filter: linear
    // lod bias 0.0, edge lod 0, min lod 0, max lod 0, unpacked 0

    // RGBA8 is stored in 4x4 tiles of 64 bytes: 16 AR pairs, then 16 GB pairs.
    uint8_t* dst = out.data() + kDataOffset;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3);
                const int y = by * 4 + (i >> 2);
                uint8_t px[4] = {0, 0, 0, 0};
                if (x < width && y < height) std::memcpy(px, rgba + (size_t(y) * width + x) * 4, 4);
                dst[i * 2 + 0] = px[3];
                dst[i * 2 + 1] = px[0];
                dst[32 + i * 2 + 0] = px[1];
                dst[32 + i * 2 + 1] = px[2];
            }
            dst += 64;
        }
    }
    return out;
}

Bytes TplMakeRgb5a3(const uint8_t* rgba, int width, int height, uint32_t wrapS, uint32_t wrapT) {
    constexpr uint32_t kDataOffset = 0x40;
    const int blocksX = (width + 3) / 4;
    const int blocksY = (height + 3) / 4;
    // Same container as TplMakeRgba8, then the header's format and the data swapped.
    std::vector<uint8_t> blank(size_t(width) * height * 4, 0);
    Bytes out = TplMakeRgba8(blank.data(), width, height, wrapS, wrapT);
    out.resize(kDataOffset + size_t(blocksX) * blocksY * 32);
    BeWrite32(out.data() + 0x14 + 4, 5);  // GX_TF_RGB5A3
    uint8_t* dst = out.data() + kDataOffset;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + (i & 3);
                const int y = by * 4 + (i >> 2);
                uint8_t px[4] = {0, 0, 0, 0};
                if (x < width && y < height) std::memcpy(px, rgba + (size_t(y) * width + x) * 4, 4);
                uint16_t v;
                if (px[3] >= 0xF0) {  // opaque: 1 RRRRR GGGGG BBBBB
                    v = uint16_t(0x8000 | ((px[0] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[2] >> 3));
                } else {              // 0 AAA RRRR GGGG BBBB
                    v = uint16_t(((px[3] >> 5) << 12) | ((px[0] >> 4) << 8) | ((px[1] >> 4) << 4) | (px[2] >> 4));
                }
                BeWrite16(dst + i * 2, v);
            }
            dst += 32;
        }
    }
    return out;
}

// =====================================================================================
// BMG
// =====================================================================================

namespace {

struct BmgSection {
    uint32_t magic = 0;
    Bytes body;  // everything after the 8-byte section header (padding included)
};

bool ParseBmg(const Bytes& bmg, uint8_t& encoding, std::vector<BmgSection>& sections) {
    if (bmg.size() < 0x20 || std::memcmp(bmg.data(), "MESGbmg1", 8) != 0) return false;
    const uint32_t count = BeRead32(bmg.data() + 0x0C);
    encoding = bmg[0x10];
    size_t pos = 0x20;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 8 > bmg.size()) return false;
        const uint32_t magic = BeRead32(bmg.data() + pos);
        const uint32_t size = BeRead32(bmg.data() + pos + 4);
        if (size < 8 || pos + size > bmg.size()) return false;
        sections.push_back({magic, Bytes(bmg.begin() + long(pos + 8), bmg.begin() + long(pos + size))});
        pos += size;
    }
    return true;
}

constexpr uint32_t Magic(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
           uint32_t(uint8_t(s[3]));
}

// Index of `messageId` in INF1 (through MID1), or -1.
int FindMessageIndex(const std::vector<BmgSection>& sections, uint32_t messageId) {
    for (const auto& s : sections) {
        if (s.magic != Magic("MID1") || s.body.size() < 8) continue;
        const uint32_t count = BeRead16(s.body.data());
        for (uint32_t i = 0; i < count && 8 + i * 4 + 4 <= s.body.size(); ++i) {
            if (BeRead32(s.body.data() + 8 + i * 4) == messageId) return int(i);
        }
    }
    return -1;
}

}  // namespace

bool BmgGetMessage(const Bytes& bmg, uint32_t messageId, std::u16string& text) {
    uint8_t encoding = 0;
    std::vector<BmgSection> sections;
    if (!ParseBmg(bmg, encoding, sections)) return false;
    const int index = FindMessageIndex(sections, messageId);
    if (index < 0) return false;
    const BmgSection* inf = nullptr;
    const BmgSection* dat = nullptr;
    for (const auto& s : sections) {
        if (s.magic == Magic("INF1")) inf = &s;
        if (s.magic == Magic("DAT1")) dat = &s;
    }
    if (!inf || !dat || inf->body.size() < 8) return false;
    const uint32_t entrySize = BeRead16(inf->body.data() + 2);
    const size_t entry = 8 + size_t(index) * entrySize;
    if (entrySize < 4 || entry + 4 > inf->body.size()) return false;
    size_t off = BeRead32(inf->body.data() + entry);
    text.clear();
    if (encoding == 2) {
        while (off + 1 < dat->body.size()) {
            const char16_t c = char16_t(BeRead16(dat->body.data() + off));
            if (c == 0) break;
            text.push_back(c);
            off += 2;
        }
    } else {
        while (off < dat->body.size() && dat->body[off]) text.push_back(char16_t(dat->body[off++]));
    }
    return true;
}

bool BmgReplaceMessage(Bytes& bmg, uint32_t messageId, const std::u16string& text) {
    uint8_t encoding = 0;
    std::vector<BmgSection> sections;
    if (!ParseBmg(bmg, encoding, sections)) return false;
    const int index = FindMessageIndex(sections, messageId);
    if (index < 0) return false;
    BmgSection* inf = nullptr;
    BmgSection* dat = nullptr;
    for (auto& s : sections) {
        if (s.magic == Magic("INF1")) inf = &s;
        if (s.magic == Magic("DAT1")) dat = &s;
    }
    if (!inf || !dat || inf->body.size() < 8) return false;
    const uint32_t entrySize = BeRead16(inf->body.data() + 2);
    const size_t entry = 8 + size_t(index) * entrySize;
    if (entrySize < 4 || entry + 4 > inf->body.size()) return false;

    // Encode. Single-byte encodings get the ASCII part of the text only.
    Bytes encoded;
    if (encoding == 2) {
        for (char16_t c : text) BePush16(encoded, uint16_t(c));
        BePush16(encoded, 0);
    } else if (encoding == 4) {
        for (char16_t c : text) {
            if (c < 0x80) {
                encoded.push_back(uint8_t(c));
            } else if (c < 0x800) {
                encoded.push_back(uint8_t(0xC0 | (c >> 6)));
                encoded.push_back(uint8_t(0x80 | (c & 0x3F)));
            } else {
                encoded.push_back(uint8_t(0xE0 | (c >> 12)));
                encoded.push_back(uint8_t(0x80 | ((c >> 6) & 0x3F)));
                encoded.push_back(uint8_t(0x80 | (c & 0x3F)));
            }
        }
        encoded.push_back(0);
    } else {
        for (char16_t c : text) {
            if (c < 0x80) encoded.push_back(uint8_t(c));
        }
        encoded.push_back(0);
    }

    // Append the new string after the existing pool (keep 2-byte alignment for UTF-16).
    Bytes& pool = dat->body;
    while (encoding == 2 && (pool.size() & 1)) pool.push_back(0);
    const uint32_t newOffset = uint32_t(pool.size());
    pool.insert(pool.end(), encoded.begin(), encoded.end());
    BeWrite32(inf->body.data() + entry, newOffset);

    Bytes out(bmg.begin(), bmg.begin() + 0x20);
    for (const auto& s : sections) {
        const size_t start = out.size();
        BePush32(out, s.magic);
        BePush32(out, 0);
        out.insert(out.end(), s.body.begin(), s.body.end());
        PadTo(out, 0x20);
        BeWrite32(out.data() + start + 4, uint32_t(out.size() - start));
    }
    BeWrite32(out.data() + 8, uint32_t(out.size()));
    bmg.swap(out);
    return true;
}

bool BmgSetMessage(Bytes& bmg, uint32_t messageId, const std::u16string& text, uint32_t attributesFrom) {
    uint8_t encoding = 0;
    std::vector<BmgSection> sections;
    if (!ParseBmg(bmg, encoding, sections)) return false;
    if (FindMessageIndex(sections, messageId) >= 0) return BmgReplaceMessage(bmg, messageId, text);
    BmgSection* inf = nullptr;
    BmgSection* mid = nullptr;
    for (auto& s : sections) {
        if (s.magic == Magic("INF1")) inf = &s;
        if (s.magic == Magic("MID1")) mid = &s;
    }
    if (!inf || !mid || inf->body.size() < 8 || mid->body.size() < 8) return false;
    const uint32_t count = BeRead16(inf->body.data());
    const uint32_t entrySize = BeRead16(inf->body.data() + 2);
    if (count != BeRead16(mid->body.data()) || entrySize < 4 || 8 + size_t(count) * entrySize > inf->body.size() ||
        8 + size_t(count) * 4 > mid->body.size() || count >= 0xFFFF) {
        return false;
    }
    // MID1 is sorted (the game binary-searches it): insert in order.
    uint32_t at = 0;
    while (at < count && BeRead32(mid->body.data() + 8 + at * 4) < messageId) ++at;
    Bytes entry(entrySize, 0);
    const int from = FindMessageIndex(sections, attributesFrom);
    if (from >= 0) {
        std::memcpy(entry.data(), inf->body.data() + 8 + size_t(from) * entrySize, entrySize);
    }
    BeWrite32(entry.data(), 0);  // string offset, set by BmgReplaceMessage below
    inf->body.insert(inf->body.begin() + long(8 + size_t(at) * entrySize), entry.begin(), entry.end());
    uint8_t id[4];
    BeWrite32(id, messageId);
    mid->body.insert(mid->body.begin() + long(8 + size_t(at) * 4), id, id + 4);
    BeWrite16(inf->body.data(), uint16_t(count + 1));
    BeWrite16(mid->body.data(), uint16_t(count + 1));
    Bytes out(bmg.begin(), bmg.begin() + 0x20);
    for (const auto& s : sections) {
        const size_t start = out.size();
        BePush32(out, s.magic);
        BePush32(out, 0);
        out.insert(out.end(), s.body.begin(), s.body.end());
        PadTo(out, 0x20);
        BeWrite32(out.data() + start + 4, uint32_t(out.size() - start));
    }
    BeWrite32(out.data() + 8, uint32_t(out.size()));
    bmg.swap(out);
    return BmgReplaceMessage(bmg, messageId, text);
}

// =====================================================================================
// BRRES / MDL0
// =====================================================================================

namespace {

struct Span {
    const uint8_t* data;
    size_t size;
    bool Has(size_t off, size_t len) const { return off <= size && len <= size - off; }
    uint32_t U32(size_t off) const { return Has(off, 4) ? BeRead32(data + off) : 0; }
    int32_t S32(size_t off) const { return int32_t(U32(off)); }
};

// nw4r ResDic: u32 size, u32 count, (count + 1) entries of 16 bytes; entry 0 is the
// root. Offsets are relative to the dictionary start.
struct DicEntry {
    size_t nameOffset;  // absolute in the span, 0 if none
    size_t dataOffset;  // absolute in the span
};

std::vector<DicEntry> ReadDic(const Span& s, size_t dic) {
    std::vector<DicEntry> entries;
    if (!s.Has(dic, 8)) return entries;
    const uint32_t count = s.U32(dic + 4);
    if (count > 100000 || !s.Has(dic + 8, (size_t(count) + 1) * 16)) return entries;
    for (uint32_t i = 1; i <= count; ++i) {
        const size_t e = dic + 8 + size_t(i) * 16;
        const int32_t name = s.S32(e + 8);
        const int32_t data = s.S32(e + 12);
        if (data == 0) continue;
        const int64_t abs = int64_t(dic) + data;
        if (abs < 0 || size_t(abs) >= s.size) continue;
        const int64_t nameAbs = int64_t(dic) + name;
        entries.push_back({(name != 0 && nameAbs > 0 && size_t(nameAbs) < s.size) ? size_t(nameAbs) : 0,
                           size_t(abs)});
    }
    return entries;
}

std::string ReadName(const Span& s, size_t off) {
    if (off == 0 || off >= s.size) return {};
    const char* p = reinterpret_cast<const char*>(s.data + off);
    return std::string(p, strnlen(p, std::min<size_t>(s.size - off, 256)));
}

// Section dictionary offsets of an MDL0 (relative to the MDL0). Indices 0..2
// (byte code, nodes, vertex positions) are the same in every revision.
size_t SectionOffset(const Span& mdl, int section) {
    const int32_t off = mdl.S32(0x10 + size_t(section) * 4);
    return (off > 0 && size_t(off) < mdl.size) ? size_t(off) : 0;
}

}  // namespace

bool IsBrres(const uint8_t* data, size_t size) {
    return size >= 0x10 && std::memcmp(data, "bres", 4) == 0;
}

std::vector<BrresModel> BrresListModels(const uint8_t* data, size_t size) {
    std::vector<BrresModel> models;
    if (!IsBrres(data, size)) return models;
    const Span s{data, size};
    const size_t root = BeRead16(data + 0x0C);
    if (!s.Has(root, 8) || std::memcmp(data + root, "root", 4) != 0) return models;
    for (const DicEntry& folder : ReadDic(s, root + 8)) {
        if (ReadName(s, folder.nameOffset) != "3DModels(NW4R)") continue;
        for (const DicEntry& model : ReadDic(s, folder.dataOffset)) {
            if (!s.Has(model.dataOffset, 0x10) || std::memcmp(data + model.dataOffset, "MDL0", 4) != 0) continue;
            const uint32_t mdlSize = s.U32(model.dataOffset + 4);
            if (!s.Has(model.dataOffset, mdlSize)) continue;
            models.push_back({ReadName(s, model.nameOffset), model.dataOffset, mdlSize});
        }
    }
    return models;
}

bool Mdl0IsValid(const uint8_t* mdl, size_t avail) {
    if (avail < 0x50 || std::memcmp(mdl, "MDL0", 4) != 0) return false;
    const uint32_t size = BeRead32(mdl + 4);
    const uint32_t version = BeRead32(mdl + 8);
    return size >= 0x50 && size <= avail && version >= 8 && version <= 12;
}

bool Mdl0Summarize(const uint8_t* mdl, size_t avail, Mdl0Summary& out) {
    if (!Mdl0IsValid(mdl, avail)) return false;
    const Span s{mdl, BeRead32(mdl + 4)};
    const Span names{mdl, avail};  // names live in the BRRES string table after the MDL0
    out = {};
    out.size = uint32_t(s.size);
    out.version = s.U32(8);

    // Info block: the first word is its own size (0x40) and the second points back
    // to the MDL0, which locates it whatever the number of section offsets.
    for (size_t pos = 0x28; pos <= 0x70; pos += 4) {
        if (s.Has(pos, 0x40) && s.U32(pos) == 0x40 && s.S32(pos + 4) == -int32_t(pos)) {
            out.vertexCount = s.U32(pos + 0x10);
            out.boxValid = true;
            for (int i = 0; i < 3; ++i) {
                out.boxMin[i] = BeReadF32(s.data + pos + 0x28 + i * 4);
                out.boxMax[i] = BeReadF32(s.data + pos + 0x34 + i * 4);
                if (!(out.boxMax[i] > out.boxMin[i]) || !(out.boxMax[i] - out.boxMin[i] < 1e6f)) {
                    out.boxValid = false;
                }
            }
            break;
        }
    }

    if (const size_t nodeDic = SectionOffset(s, 1)) {
        std::vector<std::pair<size_t, Mdl0Node>> nodes;
        for (const DicEntry& e : ReadDic(s, nodeDic)) {
            const size_t n = e.dataOffset;
            if (!s.Has(n, 0xD0)) continue;
            Mdl0Node node;
            const int32_t nameOffset = s.S32(n + 0x08);
            if (nameOffset > 0) node.name = ReadName(names, n + size_t(nameOffset));
            node.id = s.U32(n + 0x0C);
            node.matrixId = s.U32(n + 0x10);
            for (int i = 0; i < 12; ++i) node.bindMtx[i] = BeReadF32(s.data + n + 0x70 + i * 4);
            const int32_t parent = s.S32(n + 0x5C);
            node.parent = parent != 0 ? int32_t(int64_t(n) + parent) : -1;  // node offset for now
            nodes.emplace_back(n, std::move(node));
        }
        std::sort(nodes.begin(), nodes.end(),
                  [](const auto& a, const auto& b) { return a.second.id < b.second.id; });
        for (auto& entry : nodes) {
            if (entry.second.parent >= 0) {
                const size_t target = size_t(entry.second.parent);
                int32_t index = -1;
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].first == target) index = int32_t(i);
                }
                entry.second.parent = index;
            }
            out.nodes.push_back(entry.second);
        }
    }
    if (const size_t posDic = SectionOffset(s, 2)) out.vtxPosCount = uint32_t(ReadDic(s, posDic).size());
    return true;
}

uint64_t Mdl0Fingerprint(const uint8_t* mdl, size_t avail) {
    if (!Mdl0IsValid(mdl, avail)) return 0;
    const Span s{mdl, BeRead32(mdl + 4)};
    const size_t posDic = SectionOffset(s, 2);
    if (!posDic) return 0;
    uint64_t h = 0xCBF29CE484222325ull;
    int blocks = 0;
    for (const DicEntry& e : ReadDic(s, posDic)) {
        const uint32_t blockSize = s.U32(e.dataOffset);
        if (blockSize < 0x20 || !s.Has(e.dataOffset, blockSize)) continue;
        // Skip the two self-relative header words (+4 back-pointer, +8 data offset)
        // and the name offset, then hash the rest (component info, bounds, data).
        h = Fnv1a64(s.data + e.dataOffset + 0x10, blockSize - 0x10, h);
        h ^= blockSize;
        h *= 0x100000001B3ull;
        if (++blocks == 8) break;
    }
    if (blocks == 0) return 0;
    if (const size_t nodeDic = SectionOffset(s, 1)) {
        h ^= uint64_t(ReadDic(s, nodeDic).size()) << 32;
        h *= 0x100000001B3ull;
    }
    return h ? h : 1;
}

}  // namespace sonic_mkw
