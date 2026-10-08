// sonic_formats.h - the Nintendo file formats the Sonic integration has to touch.
//
// Everything here is plain host code over byte buffers (no guest memory, no GX),
// so it can be unit tested on its own:
//   Yaz0  (.szs compression)       decode + a real LZ encoder
//   U8    (.arc/.szs archives)     parse, edit file contents, rebuild
//   TPL   (UI textures)            read the first image's header, write RGBA8
//   BMG   (UI text)                replace one message by its message ID
//   BRRES/MDL0 (g3d models)        list models, read nodes, fingerprint geometry
//
// All multi-byte values in these formats are big-endian.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonic_mkw {

using Bytes = std::vector<uint8_t>;

inline uint16_t BeRead16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t BeRead32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
float BeReadF32(const uint8_t* p);
inline void BeWrite16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void BeWrite32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void BePush16(Bytes& out, uint16_t v);
void BePush32(Bytes& out, uint32_t v);

uint64_t Fnv1a64(const uint8_t* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

// ---- Yaz0 -------------------------------------------------------------------------
bool IsYaz0(const uint8_t* data, size_t size);
bool Yaz0Decode(const uint8_t* data, size_t size, Bytes& out);
Bytes Yaz0Encode(const uint8_t* data, size_t size);

// ---- U8 archive -----------------------------------------------------------------
class U8Archive {
public:
    struct Node {
        bool isDir = false;
        std::string name;
        uint32_t parent = 0;   // directories: parent node index
        uint32_t next = 0;     // directories: index one past the last node of the subtree
        Bytes data;            // files
    };

    bool Parse(const uint8_t* data, size_t size);
    Bytes Build() const;

    size_t NodeCount() const { return nodes_.size(); }
    Node& At(size_t i) { return nodes_[i]; }
    const Node& At(size_t i) const { return nodes_[i]; }
    // Full path of a node, e.g. "./message/Common.bmg"
    std::string PathOf(size_t index) const;
    std::vector<size_t> Files() const;

private:
    std::vector<Node> nodes_;
};

// ---- TPL ------------------------------------------------------------------------
struct TplInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t format = 0;
    uint32_t wrapS = 0;
    uint32_t wrapT = 0;
};
bool TplReadInfo(const uint8_t* data, size_t size, TplInfo& out);
// One RGBA8 (GX_TF_RGBA8) image from tightly packed 8-bit RGBA pixels.
Bytes TplMakeRgba8(const uint8_t* rgba, int width, int height, uint32_t wrapS = 0, uint32_t wrapT = 0);

// ---- BMG ------------------------------------------------------------------------
// Replaces the text of message `messageId` (looked up through MID1). Returns true
// when the file was changed. The text is re-encoded for the file's encoding.
bool BmgReplaceMessage(Bytes& bmg, uint32_t messageId, const std::u16string& text);
// The text of a message as UTF-16 (escape sequences kept as raw code units).
bool BmgGetMessage(const Bytes& bmg, uint32_t messageId, std::u16string& text);

// ---- BRRES / MDL0 ---------------------------------------------------------------
struct BrresModel {
    std::string name;
    size_t offset = 0;  // MDL0 start inside the BRRES
    size_t size = 0;
};
bool IsBrres(const uint8_t* data, size_t size);
std::vector<BrresModel> BrresListModels(const uint8_t* data, size_t size);

struct Mdl0Node {
    std::string name;
    uint32_t id = 0;
    uint32_t matrixId = 0;
    int32_t parent = -1;            // node index, -1 for roots
    std::array<float, 12> bindMtx{};  // node -> model space, row-major 3x4
};

struct Mdl0Summary {
    uint32_t version = 0;
    uint32_t size = 0;
    std::vector<Mdl0Node> nodes;  // sorted by id
    bool boxValid = false;
    float boxMin[3] = {0, 0, 0};
    float boxMax[3] = {0, 0, 0};
    uint32_t vertexCount = 0;
    uint32_t vtxPosCount = 0;
};

// `avail` is how many bytes may be read starting at `mdl`.
bool Mdl0IsValid(const uint8_t* mdl, size_t avail);
bool Mdl0Summarize(const uint8_t* mdl, size_t avail, Mdl0Summary& out);
// Identity of a model's geometry: a hash of its vertex position blocks, which
// nw4r never rewrites when binding (unlike material/texture references). Equal
// for the same model whether hashed from the file or from guest memory. 0 = invalid.
uint64_t Mdl0Fingerprint(const uint8_t* mdl, size_t avail);

// Splits a path or file name into lowercase alphanumeric tokens
// ("Race/Kart/sdf_kart-lg_2.szs" -> race kart sdf kart lg 2 szs).
std::vector<std::string> PathTokens(const std::string& path);
std::string ToLowerAscii(std::string text);

}  // namespace sonic_mkw
