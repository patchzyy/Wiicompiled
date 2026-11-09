#pragma once

#include "isa/big_endian.h"
#include "gx_internal.h"

// Shared GX command-stream helpers used verbatim by gx_fifo.cpp, gx_dl.cpp and gx_vertex.cpp
// instead of forked per-file copies; genuine per-caller differences are explicit parameters here.
// Header-inline throughout: the shipped clang/llvm-mingw build has no LTO across runtime shards,
// so a non-inline hot helper becomes a real call on the per-vertex path.

namespace GxCmd {

constexpr uint8_t GX_NOP_CMD         = 0x00;
constexpr uint8_t GX_LOAD_CP_REG_CMD = 0x08;
constexpr uint8_t GX_LOAD_XF_REG_CMD = 0x10;
constexpr uint8_t GX_LOAD_INDX_A_CMD = 0x20;
constexpr uint8_t GX_LOAD_INDX_D_CMD = 0x38;
constexpr uint8_t GX_CMD_CALL_DL_CMD = 0x40;
constexpr uint8_t GX_CMD_INVL_VC_CMD = 0x48;
constexpr uint8_t GX_LOAD_BP_REG_CMD = 0x61;
constexpr uint8_t GX_OPCODE_MASK_CMD = 0xF8;
constexpr uint8_t GX_VAT_MASK_CMD    = 0x07;

constexpr uint8_t GX_DRAW_QUADS_CMD          = 0x80;
// 0x88 is the second quad opcode (GX_DRAW_QUADS_2). Real hardware treats it as
// GX_QUADS as well, and MKW's nw4r shape lists do emit it.
constexpr uint8_t GX_DRAW_QUADS_2_CMD        = 0x88;
constexpr uint8_t GX_DRAW_TRIANGLES_CMD      = 0x90;
constexpr uint8_t GX_DRAW_TRIANGLE_STRIP_CMD = 0x98;
constexpr uint8_t GX_DRAW_TRIANGLE_FAN_CMD   = 0xA0;
constexpr uint8_t GX_DRAW_LINES_CMD          = 0xA8;
constexpr uint8_t GX_DRAW_LINE_STRIP_CMD     = 0xB0;
constexpr uint8_t GX_DRAW_POINTS_CMD         = 0xB8;

} // namespace GxCmd

namespace GxStream {

// --- Big-endian scalar reads out of a command stream ---------------------

inline uint16_t ReadBE16(const uint8_t* ptr) { return BigEndian::Read16(ptr); }
inline uint32_t ReadBE32(const uint8_t* ptr) { return BigEndian::Read32(ptr); }
inline float ReadBEF32(const uint8_t* ptr) { return BigEndian::ReadFloat32(ptr); }

// --- Draw opcodes ---------------------------------------------------------

inline GXPrimitive OpcodeToGXPrimitive(uint8_t opcode) {
    switch (opcode & GxCmd::GX_OPCODE_MASK_CMD) {
    case GxCmd::GX_DRAW_QUADS_CMD: return GX_QUADS;
    case GxCmd::GX_DRAW_QUADS_2_CMD: return GX_QUADS;
    case GxCmd::GX_DRAW_TRIANGLES_CMD: return GX_TRIANGLES;
    case GxCmd::GX_DRAW_TRIANGLE_STRIP_CMD: return GX_TRIANGLESTRIP;
    case GxCmd::GX_DRAW_TRIANGLE_FAN_CMD: return GX_TRIANGLEFAN;
    case GxCmd::GX_DRAW_LINES_CMD: return GX_LINES;
    case GxCmd::GX_DRAW_LINE_STRIP_CMD: return GX_LINESTRIP;
    case GxCmd::GX_DRAW_POINTS_CMD: return GX_POINTS;
    default: return GX_TRIANGLES;
    }
}

inline bool IsDrawOpcode(uint8_t opcode) {
    switch (opcode & GxCmd::GX_OPCODE_MASK_CMD) {
    case GxCmd::GX_DRAW_QUADS_CMD:
    case GxCmd::GX_DRAW_QUADS_2_CMD:
    case GxCmd::GX_DRAW_TRIANGLES_CMD:
    case GxCmd::GX_DRAW_TRIANGLE_STRIP_CMD:
    case GxCmd::GX_DRAW_TRIANGLE_FAN_CMD:
    case GxCmd::GX_DRAW_LINES_CMD:
    case GxCmd::GX_DRAW_LINE_STRIP_CMD:
    case GxCmd::GX_DRAW_POINTS_CMD:
        return true;
    default:
        return false;
    }
}

// --- Vertex layout --------------------------------------------------------

// GX_NRM_NBT3 is the one normal format whose indexed form carries three
// separate indices (normal, binormal, tangent) instead of one.
inline uint32_t NormalIndexCount(const VtxAttrFmt& fmt) {
    return fmt.cnt == GX_NRM_NBT3 ? 3u : 1u;
}

// Fixed-point fraction the CP applies to normals; it is implied by the
// component type instead of being carried in the VAT.
inline uint8_t NormalFracBits(GXCompType type) {
    switch (type) {
    case GX_U8:
        return 7;
    case GX_S8:
        return 6;
    case GX_U16:
        return 15;
    case GX_S16:
        return 14;
    default:
        return 0;
    }
}

// Components per attribute. `fallback` is what the attributes with no
// component layout (matrix indices, the XF array pseudo-attributes, GX_VA_NBT)
// report: the FIFO direct-size path treats them as one component, everything
// else treats them as "not a component attribute" and passes 0.
inline uint32_t AttrCompCount(GXAttr attr, const VtxAttrFmt& fmt, uint32_t fallback) {
    switch (attr) {
    case GX_VA_POS:
        return (fmt.cnt == GX_POS_XY) ? 2u : 3u;
    case GX_VA_NRM:
        return (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) ? 9u : 3u;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        return (fmt.cnt == GX_CLR_RGB) ? 3u : 4u;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        return (fmt.cnt == GX_TEX_S) ? 1u : 2u;
    default:
        return fallback;
    }
}

// Stream footprint of one direct color component. Note GX_RGBX8 occupies four
// bytes but only three of them are decoded.
inline uint32_t ColorByteSize(GXCompType type, GXCompCnt cnt) {
    switch (type) {
    case GX_RGB565: return 2u;
    case GX_RGB8:   return 3u;
    case GX_RGBX8:  return 4u;
    case GX_RGBA4:  return 2u;
    case GX_RGBA6:  return 3u;
    case GX_RGBA8:  return 4u;
    default:        return (cnt == GX_CLR_RGB) ? 3u : 4u;
    }
}

// Stream footprint of one GX_DIRECT attribute. `matrixAttrIsOneByte` distinguishes callers that
// walk a vertex end to end (CalcDLVertexSize, the FIFO direct-size probe) and must count the
// matrix index byte, from callers that only size component attributes and handle matrix attributes
// themselves, so must see 0. `fallbackComps` does the same for attributes with no component layout.
inline uint32_t DirectAttrByteSize(GXAttr attr, const VtxAttrFmt& fmt, bool matrixAttrIsOneByte,
                                   uint32_t fallbackComps) {
    if (IsMatrixIndexAttr(attr)) {
        return matrixAttrIsOneByte ? 1u : 0u;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        return ColorByteSize(fmt.type, fmt.cnt);
    }
    return AttrCompCount(attr, fmt, fallbackComps) * static_cast<uint32_t>(GetCompSizeBytes(fmt.type));
}

// --- Colors ---------------------------------------------------------------

// Decodes one direct/indexed color out of guest big-endian bytes. `src` must hold the bytes the
// format actually reads (see ColorByteSize; GX_RGBX8 reads three of its four). `out.a` is written
// even for GX_CLR_RGB formats, where every caller drops it (GXColor3u8) so the value is unobservable.
inline void DecodeColorBytes(const uint8_t* src, GXCompType type, GXCompCnt cnt, GXColor& out) {
    switch (type) {
    case GX_RGB565: {
        const uint16_t packed = ReadBE16(src);
        out.r = static_cast<uint8_t>(((packed >> 11) & 0x1F) * 255 / 31);
        out.g = static_cast<uint8_t>(((packed >> 5) & 0x3F) * 255 / 63);
        out.b = static_cast<uint8_t>((packed & 0x1F) * 255 / 31);
        out.a = 0xFF;
        break;
    }
    case GX_RGB8:
    case GX_RGBX8:
        out.r = src[0];
        out.g = src[1];
        out.b = src[2];
        out.a = 0xFF;
        break;
    case GX_RGBA4: {
        const uint16_t packed = ReadBE16(src);
        out.r = static_cast<uint8_t>(((packed >> 12) & 0xF) * 17);
        out.g = static_cast<uint8_t>(((packed >> 8) & 0xF) * 17);
        out.b = static_cast<uint8_t>(((packed >> 4) & 0xF) * 17);
        out.a = static_cast<uint8_t>((packed & 0xF) * 17);
        break;
    }
    case GX_RGBA6: {
        const uint32_t packed = (static_cast<uint32_t>(src[0]) << 16) |
                                (static_cast<uint32_t>(src[1]) << 8) |
                                static_cast<uint32_t>(src[2]);
        out.r = static_cast<uint8_t>(((packed >> 18) & 0x3F) * 255 / 63);
        out.g = static_cast<uint8_t>(((packed >> 12) & 0x3F) * 255 / 63);
        out.b = static_cast<uint8_t>(((packed >> 6) & 0x3F) * 255 / 63);
        out.a = static_cast<uint8_t>((packed & 0x3F) * 255 / 63);
        break;
    }
    case GX_RGBA8:
        out.r = src[0];
        out.g = src[1];
        out.b = src[2];
        out.a = src[3];
        break;
    default:
        out.r = src[0];
        out.g = src[1];
        out.b = src[2];
        out.a = (cnt == GX_CLR_RGBA) ? src[3] : 0xFF;
        break;
    }
}

// --- Components -----------------------------------------------------------

// The one fixed-point conversion table. GX_F32 is a bit pattern, never scaled;
// every other type is an integer scaled by the VAT's fractional bit count.
inline float ConvertCompToFloat(u32 raw, GXCompType type, uint8_t frac) {
    if (type == GX_F32) {
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
    float value = 0.0f;
    switch (type) {
    case GX_U8:  value = static_cast<float>(static_cast<u8>(raw)); break;
    case GX_S8:  value = static_cast<float>(static_cast<s8>(raw)); break;
    case GX_U16: value = static_cast<float>(static_cast<u16>(raw)); break;
    case GX_S16: value = static_cast<float>(static_cast<s16>(raw)); break;
    default:     value = static_cast<float>(raw); break;
    }
    if (frac > 0) {
        value /= static_cast<float>(1u << frac);
    }
    return value;
}

// Raw (unconverted, host-endian) component out of a command stream.
inline u32 ReadBECompRaw(const uint8_t* ptr, GXCompType type) {
    switch (type) {
    case GX_U8:
    case GX_S8:
        return ptr[0];
    case GX_U16:
    case GX_S16:
        return ReadBE16(ptr);
    case GX_F32:
        return ReadBE32(ptr);
    default:
        return ptr[0];
    }
}

// Raw component out of a guest vertex array.
inline u32 ReadArrayRawComp(uint32_t addr, GXCompType type) {
    switch (type) {
    case GX_U8:
    case GX_S8:
        return Memory::Read8(addr);
    case GX_U16:
    case GX_S16:
        return Memory::Read16(addr);
    case GX_F32:
        return Memory::Read32(addr);
    default:
        return Memory::Read8(addr);
    }
}

inline float ReadArrayComp(uint32_t addr, GXCompType type, uint8_t frac) {
    return ConvertCompToFloat(ReadArrayRawComp(addr, type), type, frac);
}

inline float ReadStreamComp(const uint8_t* ptr, GXCompType type, uint8_t frac) {
    return ConvertCompToFloat(ReadBECompRaw(ptr, type), type, frac);
}

// --- Indexed attributes ---------------------------------------------------

// GX_NRM_NBT3 indexed normals: three indices, each selecting one 3-component
// group, submitted as a single 9-component normal attribute.
inline void SubmitIndexedNormalNBT3(const uint32_t indices[3], const VtxAttrFmt& fmt) {
    const auto& arr = g_hleGxState.vtxArray[GX_VA_NRM];
    if (arr.base == 0 || arr.stride == 0) {
        SubmitIndexedAttribute(GX_VA_NRM, indices[0]);
        return;
    }

    const uint32_t step = static_cast<uint32_t>(GetCompSizeBytes(fmt.type));
    float comps[9]{};
    u32 rawComps[9]{};
    for (uint32_t group = 0; group < 3; ++group) {
        uint32_t addr = arr.base + indices[group] * arr.stride;
        for (uint32_t component = 0; component < 3; ++component, addr += step) {
            const uint32_t out = group * 3u + component;
            comps[out] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[out] = ReadArrayRawComp(addr, fmt.type);
        }
    }
    SubmitAttribute(GX_VA_NRM, comps, fmt, rawComps);
}

// Repacks a strided guest array into a tightly packed host copy. Only needed
// for strides aurora's GXSetArray cannot express (> 255 bytes).
inline bool PackIndexedAttrData(const uint8_t* srcBase, uint32_t stride, uint32_t count,
                                uint32_t elemSize, std::vector<uint8_t>& out) {
    if (elemSize == 0 || count == 0) return false;
    out.resize(static_cast<size_t>(count) * elemSize);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* src = srcBase + static_cast<size_t>(i) * stride;
        uint8_t* dst = out.data() + static_cast<size_t>(i) * elemSize;
        std::memcpy(dst, src, elemSize);
    }
    return true;
}

// Binds the guest XF array (position/normal matrices, lights) a GX_LOAD_INDX_*
// packet indexes into, so aurora can resolve the index itself.
inline bool ApplyIndexedXfArrayForPacket(uint8_t cmd, uint32_t value) {
    const int attr = GX_POS_MTX_ARRAY +
                     (((cmd & GxCmd::GX_OPCODE_MASK_CMD) - GxCmd::GX_LOAD_INDX_A_CMD) / 0x08);
    if (attr < GX_POS_MTX_ARRAY || attr > GX_LIGHT_ARRAY) {
        return false;
    }

    const uint32_t index = value >> 16;
    const uint32_t bytes = (((value >> 12) & 0x0fu) + 1u) * 4u;
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0 || arr.stride > 0xffu || bytes == 0) {
        return false;
    }

    const uint32_t span = index * arr.stride + bytes;
    if (!Memory::Contains(arr.base, span)) {
        return false;
    }

    const auto* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
    if (!hostPtr) {
        return false;
    }

    GXSetArray(static_cast<GXAttr>(attr), hostPtr, span, static_cast<uint8_t>(arr.stride));
    return true;
}

// --- Render state ---------------------------------------------------------

// Draw paths that bypass the guest's own GXSetAlphaCompare (raw FIFO packets,
// synthesized lyt quads) must not inherit whatever aurora happened to hold.
inline void EnsureDefaultGxAlphaCompare() {
    if (g_alphaCompareValid) {
        return;
    }
    g_alphaCompareValid = true;
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

// --- Vertex descriptor publication ---------------------------------------

// The three publish sites are NOT semantically identical and are deliberately not merged: only
// raw-FIFO publishes GX_VA_NBT (includeNbt), and immediate-mode only republishes VAT rows
// GX_VA_POS..GX_VA_TEX7 (fmtLoopFirst/Last). Indexed attributes always report GX_DIRECT here since
// all three sites expand them into the direct stream themselves before aurora sees them.
struct AuroraVtxPublishOptions {
    bool includeNbt;
    int fmtLoopFirst;
    int fmtLoopLast;
};

inline void PublishAuroraVtxState(GXVtxFmt fmt, const AuroraVtxPublishOptions& options) {
    for (int attr = 0; attr < 26; ++attr) {
        if (!options.includeNbt && attr == GX_VA_NBT) {
            continue;
        }
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_INDEX8 || type == GX_INDEX16) {
            type = GX_DIRECT;
        }
        GXSetVtxDesc(gxAttr, type);
    }

    for (int attr = options.fmtLoopFirst; attr <= options.fmtLoopLast; ++attr) {
        if (!options.includeNbt && attr == GX_VA_NBT) {
            continue;
        }
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        if (IsMatrixIndexAttr(gxAttr)) {
            continue;
        }
        const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
        GXSetVtxAttrFmt(fmt, gxAttr, f.cnt, f.type, f.frac);
    }
}

} // namespace GxStream
