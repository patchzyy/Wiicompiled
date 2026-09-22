#include "command_processor.hpp"

#include "../gfx/common.hpp"
#include "../dolphin/gx/__gx.h"
#include "../gfx/texture_replacement.hpp"
#include "dolphin/gx/GXAurora.h"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"
#include "../internal.hpp"

#include <absl/container/flat_hash_map.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

using IndexBuffer = std::vector<u16>;

static u32 prepare_idx_template(IndexBuffer& buf, GXPrimitive prim, u16 vtxCount) {
  size_t writePos = 0;
  if (prim == GX_QUADS) {
    // GX renders a three-vertex remainder as a triangle. One/two are ignored.
    const u32 completeVertices = static_cast<u32>(vtxCount) & ~3u;
    buf.resize((completeVertices / 4u) * 6u + (vtxCount % 4u == 3u ? 3u : 0u));

    for (u32 v = 0; v < completeVertices; v += 4) {
      const u16 idx0 = v;
      const u16 idx1 = static_cast<u16>(v + 1);
      const u16 idx2 = static_cast<u16>(v + 2);
      const u16 idx3 = static_cast<u16>(v + 3);
      buf[writePos++] = idx0;
      buf[writePos++] = idx1;
      buf[writePos++] = idx2;
      buf[writePos++] = idx2;
      buf[writePos++] = idx3;
      buf[writePos++] = idx0;
    }
    if (vtxCount % 4u == 3u) {
      buf[writePos++] = static_cast<u16>(completeVertices);
      buf[writePos++] = static_cast<u16>(completeVertices + 1u);
      buf[writePos++] = static_cast<u16>(completeVertices + 2u);
    }
  } else if (prim == GX_TRIANGLES) {
    const u32 completeVertices = (static_cast<u32>(vtxCount) / 3u) * 3u;
    buf.resize(completeVertices);
    for (u32 v = 0; v < completeVertices; ++v) {
      buf[writePos++] = v;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    const u32 indexCount = vtxCount < 3 ? 0u : (static_cast<u32>(vtxCount) - 2u) * 3u;
    buf.resize(indexCount);
    for (u32 v = 0; indexCount != 0 && v < vtxCount; ++v) {
      if (v < 3) {
        buf[writePos++] = v;
        continue;
      }
      buf[writePos++] = 0;
      buf[writePos++] = static_cast<u16>(v - 1);
      buf[writePos++] = v;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    const u32 indexCount = vtxCount < 3 ? 0u : (static_cast<u32>(vtxCount) - 2u) * 3u;
    buf.resize(indexCount);
    for (u32 v = 0; indexCount != 0 && v < vtxCount; ++v) {
      if (v < 3) {
        buf[writePos++] = v;
        continue;
      }
      if ((v & 1) == 0) {
        buf[writePos++] = static_cast<u16>(v - 2);
        buf[writePos++] = static_cast<u16>(v - 1);
      } else {
        buf[writePos++] = static_cast<u16>(v - 1);
        buf[writePos++] = static_cast<u16>(v - 2);
      }
      buf[writePos++] = v;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf = {0, 1, 3, 3, 2, 0};
    writePos = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  CHECK(writePos == buf.size(), "index template size mismatch ({} != {})", writePos, buf.size());
  return static_cast<u32>(writePos);
}

// Empty/incomplete draws consume FIFO bytes but cannot produce a primitive.
static bool has_complete_primitive(GXPrimitive prim, u16 count) {
  if (prim == GX_POINTS) return count >= 1;
  if (prim == GX_LINES || prim == GX_LINESTRIP) return count >= 2;
  return count >= 3;
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
static constexpr u8 CP_CMD_NOP = GX_NOP;
static constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
static constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
static constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
static constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
static constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
static constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
static constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
static constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
static constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
static constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
static constexpr u8 CP_VAT_MASK = GX_VAT_MASK;
static constexpr u8 CP_PRIMITIVE_START = 0x80;
static constexpr u8 CP_PRIMITIVE_END = 0xBF;

// Read helpers for big/little endian
#if _MSC_VER
template<typename T>
__forceinline // Yes, this was necessary.
inline T unaligned_load(const T* ptr) {
  return *static_cast<const __unaligned T*>(ptr);
}
#else
template<typename T>
inline T unaligned_load(const T* ptr) {
  T copy;
  memcpy(&copy, ptr, sizeof(T));
  return copy;
}
#endif

static inline u16 read_u16(const u8* ptr, bool bigEndian) {
  const u16 val = unaligned_load(reinterpret_cast<const u16*>(ptr));
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static inline u32 read_u32(const u8* ptr, bool bigEndian) {
  const u32 val = unaligned_load(reinterpret_cast<const u32*>(ptr));
  if (bigEndian) {
    return bswap(val);
  }
  return val;
}

static bool is_draw_cmd(u8 cmd) {
  return cmd >= CP_PRIMITIVE_START && cmd <= CP_PRIMITIVE_END;
}

static GXPrimitive primitive_from_draw_cmd(u8 cmd) {
  switch (cmd & CP_OPCODE_MASK) {
  case GX_DRAW_QUADS:
  case 0x88:
    return GX_QUADS;
  case GX_DRAW_TRIANGLES:
    return GX_TRIANGLES;
  case GX_DRAW_TRIANGLE_STRIP:
    return GX_TRIANGLESTRIP;
  case GX_DRAW_TRIANGLE_FAN:
    return GX_TRIANGLEFAN;
  case GX_DRAW_LINES:
    return GX_LINES;
  case GX_DRAW_LINE_STRIP:
    return GX_LINESTRIP;
  case GX_DRAW_POINTS:
    return GX_POINTS;
  default:
    UNLIKELY FATAL("unsupported primitive command 0x{:02X}", cmd);
  }
}




static u32 bp_get(u32 reg, u32 size, u32 shift);
static inline f32 read_f32(const u8* ptr, bool bigEndian);

static GXPixelFmt decode_pixel_fmt(u32 peCtrl, u32 cmode1) {
  switch (bp_get(peCtrl, 3, 0)) {
  case 0:
    return GX_PF_RGB8_Z24;
  case 1:
    return GX_PF_RGBA6_Z24;
  case 2:
    return GX_PF_RGB565_Z16;
  case 3:
    return GX_PF_Z24;
  case 4:
    switch (bp_get(cmode1, 2, 9)) {
    case 0:
      return GX_PF_Y8;
    case 1:
      return GX_PF_U8;
    case 2:
      return GX_PF_V8;
    default:
      Log.warn("command_processor: unsupported cmode1 pixel subtype {}", bp_get(cmode1, 2, 9));
      return GX_PF_Y8;
    }
  case 5:
    return GX_PF_YUV420;
  default:
    Log.warn("command_processor: unsupported PE pixel format {}", bp_get(peCtrl, 3, 0));
    return GX_PF_RGB8_Z24;
  }
}

static inline u64 read_u64(const u8* ptr, bool bigEndian) {
  u64 loaded;
  // Unaligned-safe load
  memcpy(&loaded, ptr, sizeof(u64));

  if (bigEndian) {
    return bswap(loaded);
  }

  return loaded;
}

struct TexBpRegMapping {
  u8 texMapId;
  enum class Kind : uint8_t { Mode0, Mode1, Image0, Image1, Image2, Image3, Tlut } kind;
};

static std::optional<TexBpRegMapping> decode_tex_bp_reg(u32 regId) {
  constexpr std::array mode0Ids{0x80u, 0x81u, 0x82u, 0x83u, 0xA0u, 0xA1u, 0xA2u, 0xA3u};
  constexpr std::array mode1Ids{0x84u, 0x85u, 0x86u, 0x87u, 0xA4u, 0xA5u, 0xA6u, 0xA7u};
  constexpr std::array image0Ids{0x88u, 0x89u, 0x8Au, 0x8Bu, 0xA8u, 0xA9u, 0xAAu, 0xABu};
  constexpr std::array image1Ids{0x8Cu, 0x8Du, 0x8Eu, 0x8Fu, 0xACu, 0xADu, 0xAEu, 0xAFu};
  constexpr std::array image2Ids{0x90u, 0x91u, 0x92u, 0x93u, 0xB0u, 0xB1u, 0xB2u, 0xB3u};
  constexpr std::array image3Ids{0x94u, 0x95u, 0x96u, 0x97u, 0xB4u, 0xB5u, 0xB6u, 0xB7u};
  constexpr std::array tlutIds{0x98u, 0x99u, 0x9Au, 0x9Bu, 0xB8u, 0xB9u, 0xBAu, 0xBBu};

  for (u8 i = 0; i < MaxTextures; ++i) {
    if (regId == mode0Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Mode0};
    }
    if (regId == mode1Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Mode1};
    }
    if (regId == image0Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image0};
    }
    if (regId == image1Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image1};
    }
    if (regId == image2Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image2};
    }
    if (regId == image3Ids[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Image3};
    }
    if (regId == tlutIds[i]) {
      return TexBpRegMapping{.texMapId = i, .kind = TexBpRegMapping::Kind::Tlut};
    }
  }
  return std::nullopt;
}

// Helper to convert packed RGBA8 to Vec4<float>
static Vec4<float> unpack_color(u32 packed) {
  return {
      static_cast<float>(packed >> 24 & 0xFF) / 255.f,
      static_cast<float>(packed >> 16 & 0xFF) / 255.f,
      static_cast<float>(packed >> 8 & 0xFF) / 255.f,
      static_cast<float>(packed & 0xFF) / 255.f,
  };
}

static inline f32 read_f32(const u8* ptr, bool bigEndian) {
  u32 bits = read_u32(ptr, bigEndian);
  f32 val;
  std::memcpy(&val, &bits, sizeof(val));
  return val;
}

// Marks draw state dirty *and* invalidates the resolved-pipeline memo.
static inline void mark_pipeline_state_dirty() noexcept {
  g_gxState.stateDirty = true;
  g_gxState.pipelineStateGeneration = next_gx_state_epoch();
}

// Rejects a malformed XF write in release builds.
#define XF_REQUIRE(cond, msg, ...)                                                                                     \
  do {                                                                                                                 \
    if (!(cond)) UNLIKELY {                                                                                            \
      CHECK(cond, msg, ##__VA_ARGS__);                                                                                  \
      Log.warn(msg, ##__VA_ARGS__);                                                                                     \
      return true;                                                                                                     \
    }                                                                                                                  \
  } while (0)

static bool copy_xf_data(u32 addr, const u8* data, u32 len, bool bigEndian) {
  if (addr < 0x78) {
    // Position matrices (0x0000 - 0x0077)
    u32 mtxIdx = addr / 12;
    u32 startOffset = addr % 12;
    // We only support full writes to matrices
    XF_REQUIRE(mtxIdx < MaxPnMtx, "XF: PosMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 12, "XF: PosMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].pos;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr < 0x0F0) {
    // Texture matrices (0x078-0x0EF)
    u32 texBase = addr - 0x078;
    u32 mtxIdx = texBase / 12;
    u32 startOffset = texBase % 12;
    XF_REQUIRE(mtxIdx < MaxTexMtx, "XF TexMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && (len == 8 || len == 12), "XF TexMtx sub-copy unsupported: offs={}, len={}",
               startOffset, len);

    // Determine if 2x4 or 3x4 from count
    auto& mtx = g_gxState.texMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x400 && addr < 0x45A) {
    // Normal matrices (0x400-0x459)
    u32 nrmBase = addr - 0x400;
    u32 mtxIdx = nrmBase / 9;
    u32 startOffset = nrmBase % 9;
    // We only support full writes to matrices
    XF_REQUIRE(mtxIdx < MaxPnMtx, "XF: NrmMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 9, "XF: NrmMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.pnMtx[mtxIdx].nrm;
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      u32 xfIdx = i;
      u32 row = xfIdx / 3;
      u32 col = xfIdx % 3;
      if (row < 3) {
        flat[row * 4 + col] = read_f32(data + i * 4, bigEndian);
      }
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x500 && addr < 0x5F0) {
    // Post-transform texture matrices (0x500-0x5EF)
    u32 ptBase = addr - 0x500;
    u32 mtxIdx = ptBase / 12;
    u32 startOffset = ptBase % 12;
    XF_REQUIRE(mtxIdx < MaxPTTexMtx, "XF: PTTexMtx copy oob; mtxIdx={}", mtxIdx);
    XF_REQUIRE(startOffset == 0 && len == 12, "XF: PTTexMtx sub-copy unsupported: offs={}, len={}", startOffset, len);
    auto& mtx = g_gxState.ptTexMtxs[mtxIdx];
    f32* flat = reinterpret_cast<f32*>(&mtx);
    for (u32 i = 0; i < len; i++) {
      flat[startOffset + i] = read_f32(data + i * 4, bigEndian);
    }
    g_gxState.stateDirty = true;
    return true;
  } else if (addr >= 0x600 && addr < 0x680) {
    // Lights (0x600-0x67F) - 8 lights, 16 values each
    u32 lightBase = addr - 0x600;
    u32 lightIdx = lightBase / 0x10;
    u32 startOffset = lightBase % 0x10;
    XF_REQUIRE(lightIdx < GX::MaxLights, "XF: Light copy oob; lightIdx={}", lightIdx);
    XF_REQUIRE(startOffset + len <= 0x10,
               "XF: Light copy that crosses across light boundaries unsupported: offs={}, len={}", startOffset, len);
    auto& light = g_gxState.lights[lightIdx];
    for (u32 i = 0; i < len; i++) {
      u32 field = startOffset + i;
      f32 val = read_f32(data + i * 4, bigEndian);
      u32 ival = read_u32(data + i * 4, bigEndian);
      switch (field) {
      case 3: // Color (packed u32)
        light.color = unpack_color(ival);
        break;
      case 4:
        light.cosAtt[0] = val;
        break; // a0
      case 5:
        light.cosAtt[1] = val;
        break; // a1
      case 6:
        light.cosAtt[2] = val;
        break; // a2
      case 7:
        light.distAtt[0] = val;
        break; // k0
      case 8:
        light.distAtt[1] = val;
        break; // k1
      case 9:
        light.distAtt[2] = val;
        break; // k2
      case 10:
        light.pos[0] = val;
        break; // px
      case 11:
        light.pos[1] = val;
        break; // py
      case 12:
        light.pos[2] = val;
        break; // pz
      case 13:
        light.dir[0] = val;
        break; // nx
      case 14:
        light.dir[1] = val;
        break; // ny
      case 15:
        light.dir[2] = val;
        break; // nz
      default:
        break; // padding (0-2)
      }
    }
    g_gxState.preparedLightsDirty = true;
    g_gxState.stateDirty = true;
    return true;
  }
  return false;
}

static void apply_xf_viewport() {
  const auto& vp = g_gxState.xfViewport;
  const f32 sx = vp[0];
  const f32 sy = vp[1];
  const f32 sz = vp[2];
  const f32 ox = vp[3];
  const f32 oy = vp[4];
  const f32 oz = vp[5];
  const f32 width = sx * 2.0f;
  const f32 height = -sy * 2.0f;
  constexpr f32 z24Scale = 16777216.0f;

  set_logical_viewport({
      .left = ox - 340.0f - width / 2.0f,
      .top = oy - 340.0f - height / 2.0f,
      .width = width,
      .height = height,
      .znear = (oz - sz) / z24Scale,
      .zfar = oz / z24Scale,
  });
}

static void apply_xf_projection() {
  const auto& raw = g_gxState.xfProjection;
  auto& proj = g_gxState.proj;
  proj = {};
  proj.m0[0] = raw[0];
  proj.m1[1] = raw[2];
  proj.m2[2] = raw[4];
  proj.m2[3] = raw[5];

  if (g_gxState.projType == GX_ORTHOGRAPHIC) {
    proj.m0[3] = raw[1];
    proj.m1[3] = raw[3];
    proj.m3[3] = 1.0f;
  } else {
    proj.m0[2] = raw[1];
    proj.m1[2] = raw[3];
    proj.m3[2] = -1.0f;
  }

  g_gxState.stateDirty = true;
}

// Forward declarations for register handlers
static void handle_bp(u32 value, bool bigEndian);
static void handle_cp(u8 addr, u32 value, bool bigEndian);
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian);
static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian);
static bool handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian);

uint32_t process(const u8* data, u32 size, bool bigEndian) {
  ZoneScoped;
  // Everything decoded here mutates renderer state (GX state, the recorded command lists and the mapped staging buffers), so take the renderer GPU mutex once for the whole drain rather than once per draw command.
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  u32 pos = 0;

  while (pos < size) {
    const u32 commandStart = pos;
    u8 cmd = data[pos++];
    u8 opcode = cmd & CP_OPCODE_MASK;
    // Log.warn("Processing opcode {:02x} at pos {} (size {})", opcode, pos - 1, size);

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      CHECK(pos + 4 <= size, "BP reg read overrun");
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_bp(value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      CHECK(pos + 5 <= size, "CP reg read overrun");
      u8 addr = data[pos++];
      u32 value = read_u32(data + pos, bigEndian);
      pos += 4;
      handle_cp(addr, value, bigEndian);
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      handle_xf(data, pos, size, bigEndian);
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      CHECK(pos + 4 <= size, "indexed XF read overrun");
      const u32 value = read_u32(data + pos, bigEndian);
      pos += 4;

      const u32 arrayType = GX_POS_MTX_ARRAY + ((opcode - CP_CMD_LOAD_INDX_A) / 0x08);
      const u32 srcArrayIdx = value >> 16;
      const u16 len = static_cast<u16>(((value >> 12) & 0x0f) + 1);
      const u16 dstAddr = static_cast<u16>(value & 0x0fff);
      auto const& array = g_gxState.arrays[arrayType];
      const u32 byteOffset = srcArrayIdx * array.stride;
      const u32 byteCount = static_cast<u32>(len) * 4u;
      if (array.data == nullptr || array.stride == 0 || byteOffset + byteCount > array.size) {
        static u32 invalidIndexedXfLogCount = 0;
        if (invalidIndexedXfLogCount < 16) {
          Log.warn("Skipping indexed XF load with invalid source array: array={} idx={} stride={} offset={} bytes={} "
                   "size={} dst=0x{:04X}",
                   arrayType, srcArrayIdx, array.stride, byteOffset, byteCount, array.size, dstAddr);
          ++invalidIndexedXfLogCount;
        }
        break;
      }
      u8* srcData = ((u8*)array.data) + byteOffset;
      if (!copy_xf_data(dstAddr, srcData, len, bigEndian)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=0x{:04X})", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      CHECK(pos + 8 <= size, "call DL read overrun");
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      pos += 8;
      break;
    }

    case CP_CMD_INVAL_VTX: {
      // GXInvalidateVtxCache tells the GPU that CPU-written indexed vertex arrays must be observed by subsequent draws.
      for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
        g_gxState.arrays[i].cachedRange = {};
      }
      // A merged draw retains its previous array uploads. Force a new draw so
      // handle_draw_unmerged observes the invalidation and uploads fresh data.
      // Pipeline configuration itself did not change.
      g_gxState.stateDirty = true;
      break;
    }

    case GX_LOAD_AURORA: {
      if (!handle_aurora(data, pos, size, bigEndian)) {
        return size;
      }
      break;
    }

    default:
      // Draw commands occupy the full 0x80-0xBF range.
      if (is_draw_cmd(cmd)) {
        try {
          if (!handle_draw(cmd, data, pos, size, bigEndian)) return size;
        } catch (const gfx::StagingBatchFull&) {
          return commandStart;
        }
      } else {
        static u32 unknownLogCount = 0;
        if (unknownLogCount < 16) {
          // Hex dump surrounding bytes for debugging
          u32 dumpStart = (pos > 17) ? pos - 17 : 0;
          u32 dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (u32 i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.warn("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
          Log.warn("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, pos - 1);
          ++unknownLogCount;
        }
      }
      break;
    }
  }
  return size;
}

// Helper to extract bit fields from a 32-bit register
inline static u32 bp_get(u32 reg, u32 size, u32 shift) { return reg >> shift & (1u << size) - 1; }

static u8 normal_frac_bits(GXCompType type) {
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

static void refresh_copy_filter_flags() {
  bool aa = false;
  for (const auto& sample : g_gxState.copyFilterSamplePattern) {
    aa |= sample[0] != 6 || sample[1] != 6;
  }
  g_gxState.copyFilterAa = aa;

  static constexpr std::array<u8, 7> DefaultVFilter{0, 0, 21, 22, 21, 0, 0};
  bool vf = false;
  for (size_t i = 0; i < DefaultVFilter.size(); ++i) {
    vf |= g_gxState.copyFilterVFilter[i] != DefaultVFilter[i];
  }
  g_gxState.copyFilterVf = vf;
}

// BP register handler - decodes BP (RAS/pixel engine) register writes and updates g_gxState
static void handle_bp(u32 value, bool bigEndian) {
  u32 regId = (value >> 24) & 0xFF;
  // Mask off the register ID from the value for field extraction
  // (the regId is stored in bits 24-31, data is in bits 0-23)

  if (regId == 0xFE) {
    g_gxState.bpRegCache[regId] = value & 0x00FFFFFF;
    return;
  } else {
    const u32 ssMask = g_gxState.bpRegCache[0xFE];
    // A preceding 0xFE write is rare; the common path only has to prove the mask is already wide open.
    if (ssMask != 0x00FFFFFF) UNLIKELY {
      g_gxState.bpRegCache[0xFE] = 0x00FFFFFF;
    }
    const u32 merged = (g_gxState.bpRegCache[regId] & ~ssMask) | (value & ssMask);
    value = (regId << 24) | (merged & 0x00FFFFFF);
    if (g_gxState.bpRegCache[regId] == value) return;
    g_gxState.bpRegCache[regId] = value;
  }
  // TEV color combiner stages (0xC0, 0xC2, 0xC4, ... 0xDE)
  if (regId >= 0xC0 && regId <= 0xDE && (regId & 1) == 0) {
    u32 stage = (regId - 0xC0) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.colorPass.d = static_cast<GXTevColorArg>(bp_get(value, 4, 0));
      s.colorPass.c = static_cast<GXTevColorArg>(bp_get(value, 4, 4));
      s.colorPass.b = static_cast<GXTevColorArg>(bp_get(value, 4, 8));
      s.colorPass.a = static_cast<GXTevColorArg>(bp_get(value, 4, 12));
      s.colorOp.clamp = bp_get(value, 1, 19) != 0;
      s.colorOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        // Bias==3 means compare mode: reconstruct GXTevOp enum (8 + 3-bit hw value)
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.colorOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.colorOp.bias = GX_TB_ZERO;
        s.colorOp.scale = GX_CS_SCALE_1;
      } else {
        // Normal mode: bit18 is op (0=ADD, 1=SUB), bits16-17 is bias, bits20-21 is scale
        s.colorOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.colorOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.colorOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  // TEV alpha combiner stages (0xC1, 0xC3, 0xC5, ... 0xDF)
  if (regId >= 0xC1 && regId <= 0xDF && (regId & 1) == 1) {
    u32 stage = (regId - 0xC1) / 2;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.tevSwapRas = static_cast<GXTevSwapSel>(bp_get(value, 2, 0));
      s.tevSwapTex = static_cast<GXTevSwapSel>(bp_get(value, 2, 2));
      s.alphaPass.d = static_cast<GXTevAlphaArg>(bp_get(value, 3, 4));
      s.alphaPass.c = static_cast<GXTevAlphaArg>(bp_get(value, 3, 7));
      s.alphaPass.b = static_cast<GXTevAlphaArg>(bp_get(value, 3, 10));
      s.alphaPass.a = static_cast<GXTevAlphaArg>(bp_get(value, 3, 13));
      s.alphaOp.clamp = bp_get(value, 1, 19) != 0;
      s.alphaOp.outReg = static_cast<GXTevRegID>(bp_get(value, 2, 22));
      if (bp_get(value, 2, 16) == 3) {
        u32 hwOp = bp_get(value, 1, 18) | (bp_get(value, 2, 20) << 1);
        s.alphaOp.op = static_cast<GXTevOp>(hwOp + 8);
        s.alphaOp.bias = GX_TB_ZERO;
        s.alphaOp.scale = GX_CS_SCALE_1;
      } else {
        s.alphaOp.op = static_cast<GXTevOp>(bp_get(value, 1, 18));
        s.alphaOp.bias = static_cast<GXTevBias>(bp_get(value, 2, 16));
        s.alphaOp.scale = static_cast<GXTevScale>(bp_get(value, 2, 20));
      }
      mark_pipeline_state_dirty();
    }
    return;
  }

  switch (regId) {
  // genMode (0x00)
  case 0x00: {
    g_gxState.numTexGens = bp_get(value, 4, 0);
    g_gxState.numChans = bp_get(value, 3, 4);
    // genMode owns the same numTexGens/numChans that XF 0x3F/0x09 decode, so the XF cache can no longer vouch for those two slots.
    g_gxState.invalidateXfReg(0x3F);
    g_gxState.invalidateXfReg(0x09);
    g_gxState.numTevStages = bp_get(value, 4, 10) + 1;
    u32 hwCull = bp_get(value, 2, 14);
    // Swap front/back to match GX convention
    switch (hwCull) {
    case GX_CULL_FRONT:
      g_gxState.cullMode = GX_CULL_BACK;
      break;
    case GX_CULL_BACK:
      g_gxState.cullMode = GX_CULL_FRONT;
      break;
    default:
      g_gxState.cullMode = static_cast<GXCullMode>(hwCull);
      break;
    }
    g_gxState.numIndStages = bp_get(value, 3, 16);
    mark_pipeline_state_dirty();
    break;
  }

  // Display copy sample pattern (0x01-0x04), six 4-bit samples per BP reg.
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x04: {
    const size_t first = static_cast<size_t>(regId - 0x01) * 6;
    for (size_t i = 0; i < 6; ++i) {
      const size_t sample = first + i;
      g_gxState.copyFilterSamplePattern[sample / 2][sample % 2] = static_cast<u8>(bp_get(value, 4, i * 4));
    }
    refresh_copy_filter_flags();
    break;
  }

  // Indirect texture mask (0x0F).
  case 0x0F:
    g_gxState.indTexMask = static_cast<u8>(value & 0xFF);
    g_gxState.stateDirty = true;
    break;

  // TEV indirect stages (0x10-0x1F)
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1A:
  case 0x1B:
  case 0x1C:
  case 0x1D:
  case 0x1E:
  case 0x1F: {
    u32 stage = regId - 0x10;
    if (stage < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage];
      s.indTexStage = static_cast<GXIndTexStageID>(bp_get(value, 2, 0));
      s.indTexFormat = static_cast<GXIndTexFormat>(bp_get(value, 2, 2));
      s.indTexBiasSel = static_cast<GXIndTexBiasSel>(bp_get(value, 3, 4));
      s.indTexAlphaSel = static_cast<GXIndTexAlphaSel>(bp_get(value, 2, 7));
      s.indTexMtxId = static_cast<GXIndTexMtxID>(bp_get(value, 4, 9));
      s.indTexWrapS = static_cast<GXIndTexWrap>(bp_get(value, 3, 13));
      s.indTexWrapT = static_cast<GXIndTexWrap>(bp_get(value, 3, 16));
      s.indTexUseOrigLOD = bp_get(value, 1, 19) != 0;
      s.indTexAddPrev = bp_get(value, 1, 20) != 0;
      mark_pipeline_state_dirty();
    }
    break;
  }

  // Scissor registers (0x20, 0x21)
  case 0x20:
  case 0x21: {
    const u32 scis0 = g_gxState.bpRegCache[0x20];
    const u32 scis1 = g_gxState.bpRegCache[0x21];
    const int32_t tp = static_cast<int32_t>(bp_get(scis0, 11, 0)) - 342;
    const int32_t lf = static_cast<int32_t>(bp_get(scis0, 11, 12)) - 342;
    const int32_t bm = static_cast<int32_t>(bp_get(scis1, 11, 0)) - 342;
    const int32_t rt = static_cast<int32_t>(bp_get(scis1, 11, 12)) - 342;
    const int32_t wd = std::max(rt - lf + 1, 0);
    const int32_t ht = std::max(bm - tp + 1, 0);
    set_logical_scissor({lf, tp, wd, ht});
    break;
  }

  // Line/point size (0x22)
  case 0x22: {
    g_gxState.lineWidth = static_cast<u8>(bp_get(value, 8, 0));
    g_gxState.pointSize = static_cast<u8>(bp_get(value, 8, 8));
    g_gxState.lineTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 16));
    g_gxState.pointTexOffset = static_cast<GXTexOffset>(bp_get(value, 3, 19));
    g_gxState.lineHalfAspect = bp_get(value, 1, 22) != 0;
    g_gxState.stateDirty = true;
    break;
  }

  // Indirect texture scale (0x25, 0x26)
  case 0x25: {
    if (MaxIndStages > 0) {
      g_gxState.indStages[0].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[0].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 1) {
      g_gxState.indStages[1].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[1].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }
  case 0x26: {
    if (MaxIndStages > 2) {
      g_gxState.indStages[2].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 0));
      g_gxState.indStages[2].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 4));
    }
    if (MaxIndStages > 3) {
      g_gxState.indStages[3].scaleS = static_cast<GXIndTexScale>(bp_get(value, 4, 8));
      g_gxState.indStages[3].scaleT = static_cast<GXIndTexScale>(bp_get(value, 4, 12));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Indirect texture reference (0x27)
  case 0x27: {
    for (u32 i = 0; i < MaxIndStages; i++) {
      g_gxState.indStages[i].texMapId = static_cast<GXTexMapID>(bp_get(value, 3, i * 6));
      g_gxState.indStages[i].texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, i * 6 + 3));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // TEV order / tref (0x28-0x2F) - 2 stages per register
  case 0x28:
  case 0x29:
  case 0x2A:
  case 0x2B:
  case 0x2C:
  case 0x2D:
  case 0x2E:
  case 0x2F: {
    u32 idx = regId - 0x28;
    u32 stage0 = idx * 2;
    u32 stage1 = idx * 2 + 1;

    // Channel ID reverse mapping from hardware to GX
    static const GXChannelID r2c[] = {GX_COLOR0A0, GX_COLOR1A1,   GX_COLOR0A0,    GX_COLOR1A1,
                                      GX_COLOR0A0, GX_ALPHA_BUMP, GX_ALPHA_BUMPN, GX_COLOR_ZERO};

    if (stage0 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage0];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 0));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 3));
      // bit 6 = tex enable
      if (!bp_get(value, 1, 6)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 7);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    if (stage1 < MaxTevStages) {
      auto& s = g_gxState.tevStages[stage1];
      s.texMapId = static_cast<GXTexMapID>(bp_get(value, 3, 12));
      s.texCoordId = static_cast<GXTexCoordID>(bp_get(value, 3, 15));
      if (!bp_get(value, 1, 18)) {
        s.texMapId = GX_TEXMAP_NULL;
      }
      u32 chanHw = bp_get(value, 3, 19);
      s.channelId = (chanHw < 8) ? r2c[chanHw] : GX_COLOR_NULL;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Z mode (0x40)
  case 0x40: {
    g_gxState.depthCompare = bp_get(value, 1, 0) != 0;
    g_gxState.depthFunc = static_cast<GXCompare>(bp_get(value, 3, 1));
    g_gxState.depthUpdate = bp_get(value, 1, 4) != 0;
    mark_pipeline_state_dirty();
    break;
  }

  // Blend mode / cmode0 (0x41)
  case 0x41: {
    bool blendEn = bp_get(value, 1, 0) != 0;
    bool logicEn = bp_get(value, 1, 1) != 0;
    bool dither = bp_get(value, 1, 2) != 0;
    g_gxState.colorUpdate = bp_get(value, 1, 3) != 0;
    g_gxState.alphaUpdate = bp_get(value, 1, 4) != 0;
    g_gxState.blendFacDst = static_cast<GXBlendFactor>(bp_get(value, 3, 5));
    g_gxState.blendFacSrc = static_cast<GXBlendFactor>(bp_get(value, 3, 8));
    bool subtract = bp_get(value, 1, 11) != 0;
    g_gxState.blendOp = static_cast<GXLogicOp>(bp_get(value, 4, 12));

    if (subtract) {
      g_gxState.blendMode = GX_BM_SUBTRACT;
    } else if (blendEn) {
      g_gxState.blendMode = GX_BM_BLEND;
    } else if (logicEn) {
      g_gxState.blendMode = GX_BM_LOGIC;
    } else {
      g_gxState.blendMode = GX_BM_NONE;
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Dst alpha / cmode1 (0x42)
  case 0x42: {
    u8 alpha = bp_get(value, 8, 0);
    bool enabled = bp_get(value, 1, 8) != 0;
    g_gxState.dstAlpha = enabled ? alpha : UINT32_MAX;
    g_gxState.pixelFmt = decode_pixel_fmt(g_gxState.bpRegCache[0x43], value);
    mark_pipeline_state_dirty();
    break;
  }

  // PE control (0x43) - pixel format, z format, zcomp location
  case 0x43: {
    g_gxState.pixelFmt = decode_pixel_fmt(value, g_gxState.bpRegCache[0x42]);
    g_gxState.zFmt = static_cast<GXZFmt16>(bp_get(value, 3, 3));
    g_gxState.zCompLocBeforeTex = bp_get(value, 1, 6) != 0;
    mark_pipeline_state_dirty();
    break;
  }
  case 0x44:
    g_gxState.fieldMask = value & 0x3u;
    break;

  // Bounding box clear/update registers (0x55, 0x56)
  case 0x55:
  case 0x56: {
    const u32 offset = (regId & 2u);
    g_gxState.boundingBox[offset] = static_cast<u16>(value & 0x3ffu);
    g_gxState.boundingBox[offset + 1] = static_cast<u16>((value >> 10) & 0x3ffu);
    break;
  }
  case 0x58:
    g_gxState.revBits = value & 0x00FFFFFFu;
    break;

  // Scissor box offset (0x59)
  case 0x59: {
    g_gxState.scissorOffsetX = static_cast<s32>(((value & 0x3ffu) << 1) - 0x156u);
    g_gxState.scissorOffsetY = static_cast<s32>(((value & 0xffc00u) >> 9) - 0x156u);
    set_logical_scissor(g_gxState.logicalScissor);
    break;
  }

  // TLUT load address / execute (0x64, 0x65)
  case 0x64:
    break;
  case 0x65: {
    const auto idx = bp_get(value, 10, 0);
    if (idx < MaxTluts) {
      auto& slot = g_gxState.loadedTluts[idx];
      slot.loadTlut0 = g_gxState.bpRegCache[0x64];
      slot.numEntries = static_cast<u16>(bp_get(value, 10, 10) + 1);
    }
    break;
  }
  case 0x68:
    g_gxState.fieldMode = value & 0x1u;
    break;

  // Alpha compare (0xF3)
  case 0xF3: {
    g_gxState.alphaCompare.ref0 = bp_get(value, 8, 0);
    g_gxState.alphaCompare.ref1 = bp_get(value, 8, 8);
    g_gxState.alphaCompare.comp0 = static_cast<GXCompare>(bp_get(value, 3, 16));
    g_gxState.alphaCompare.comp1 = static_cast<GXCompare>(bp_get(value, 3, 19));
    g_gxState.alphaCompare.op = static_cast<GXAlphaOp>(bp_get(value, 2, 22));
    mark_pipeline_state_dirty();
    break;
  }

  // TEV K color/alpha select (0xF6-0xFD)
  case 0xF6:
  case 0xF7:
  case 0xF8:
  case 0xF9:
  case 0xFA:
  case 0xFB:
  case 0xFC:
  case 0xFD: {
    u32 kselIdx = regId - 0xF6;
    // Swap table entries (packed into pairs of ksel registers)
    if (kselIdx < MaxTevSwap * 2) {
      u32 swapIdx = kselIdx / 2;
      if (kselIdx & 1) {
        g_gxState.tevSwapTable[swapIdx].blue = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].alpha = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      } else {
        g_gxState.tevSwapTable[swapIdx].red = static_cast<GXTevColorChan>(bp_get(value, 2, 0));
        g_gxState.tevSwapTable[swapIdx].green = static_cast<GXTevColorChan>(bp_get(value, 2, 2));
      }
    }
    // K color/alpha selection for 2 stages per register
    u32 stage0 = kselIdx * 2;
    u32 stage1 = kselIdx * 2 + 1;
    if (stage0 < MaxTevStages) {
      g_gxState.tevStages[stage0].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 4));
      g_gxState.tevStages[stage0].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 9));
    }
    if (stage1 < MaxTevStages) {
      g_gxState.tevStages[stage1].kcSel = static_cast<GXTevKColorSel>(bp_get(value, 5, 14));
      g_gxState.tevStages[stage1].kaSel = static_cast<GXTevKAlphaSel>(bp_get(value, 5, 19));
    }
    mark_pipeline_state_dirty();
    break;
  }

  // Fog A/B parameters (0xEE-0xF0)
  // FOG0 (0xEE): A parameter - sign(1)|exp(8)|mantissa(11) partial IEEE 754 float
  case 0xEE: {
    g_gxState.fog.fog0Raw = value;
    u32 a_mant = bp_get(value, 11, 0);
    u32 a_exp = bp_get(value, 8, 11);
    u32 a_sign = bp_get(value, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    std::memcpy(&g_gxState.fog.aRaw, &a_bits, sizeof(g_gxState.fog.aRaw));
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    g_gxState.fog.a = std::ldexp(g_gxState.fog.aRaw, static_cast<int>(b_s));
    g_gxState.stateDirty = true;
    break;
  }
  // FOG1 (0xEF): B mantissa (24-bit)
  case 0xEF: {
    g_gxState.fog.fog1Raw = value;
    g_gxState.fog.bMagnitude = bp_get(value, 24, 0);
    u32 b_s = g_gxState.fog.fog2Raw & 0x1F;
    float B_mant = static_cast<float>(g_gxState.fog.bMagnitude) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    g_gxState.stateDirty = true;
    break;
  }
  // FOG2 (0xF0): B shift/exponent (5-bit)
  case 0xF0: {
    g_gxState.fog.fog2Raw = value;
    u32 b_s = bp_get(value, 5, 0);
    g_gxState.fog.bShift = b_s;
    u32 a_mant = bp_get(g_gxState.fog.fog0Raw, 11, 0);
    u32 a_exp = bp_get(g_gxState.fog.fog0Raw, 8, 11);
    u32 a_sign = bp_get(g_gxState.fog.fog0Raw, 1, 19);
    u32 a_bits = (a_sign << 31) | (a_exp << 23) | (a_mant << 12);
    std::memcpy(&g_gxState.fog.aRaw, &a_bits, sizeof(g_gxState.fog.aRaw));
    g_gxState.fog.a = std::ldexp(g_gxState.fog.aRaw, static_cast<int>(b_s));
    g_gxState.fog.bMagnitude = bp_get(g_gxState.fog.fog1Raw, 24, 0);
    float B_mant = static_cast<float>(g_gxState.fog.bMagnitude) / 8388638.0f;
    g_gxState.fog.b = std::ldexp(B_mant, static_cast<int>(b_s) - 1);
    g_gxState.stateDirty = true;
    break;
  }

  // Fog type + C parameter from FOG3 (0xF1)
  case 0xF1: {
    const u32 fogFunc = bp_get(value, 3, 21);
    const u32 fogProj = bp_get(value, 1, 20);
    GXFogType fogType = static_cast<GXFogType>(fogFunc | (fogProj << 3));
    g_gxState.fog.type = fogType;
    // Decode C parameter (same partial float encoding as A)
    u32 c_mant = bp_get(value, 11, 0);
    u32 c_exp = bp_get(value, 8, 11);
    u32 c_sign = bp_get(value, 1, 19);
    u32 c_bits = (c_sign << 31) | (c_exp << 23) | (c_mant << 12);
    std::memcpy(&g_gxState.fog.c, &c_bits, sizeof(g_gxState.fog.c));
    mark_pipeline_state_dirty();
    break;
  }

  // Fog color from FOGCLR (0xF2)
  case 0xF2: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    u8 r = bp_get(value, 8, 16);
    g_gxState.fog.color = {
        static_cast<float>(r) / 255.f,
        static_cast<float>(g) / 255.f,
        static_cast<float>(b) / 255.f,
        1.f,
    };
    g_gxState.stateDirty = true;
    break;
  }

  // TEV and K color registers (0xE0-0xE7): even are RA, odd are BG.
  // Bit 23 selects a K color register over a TEV color register.
  case 0xE0:
  case 0xE1:
  case 0xE2:
  case 0xE3:
  case 0xE4:
  case 0xE5:
  case 0xE6:
  case 0xE7: {
    u32 idx = (regId - 0xE0) / 2;
    bool isRA = (regId & 1) == 0;
    bool isKColor = bp_get(value, 1, 23) != 0;

    if (isKColor) {
      // K color register (8-bit components)
      if (idx < GX_MAX_KCOLOR) {
        auto& kc = g_gxState.kcolors[idx];
        if (isRA) {
          kc[0] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // R
          kc[3] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // A
        } else {
          kc[2] = static_cast<float>(bp_get(value, 8, 0)) / 255.f;  // B
          kc[1] = static_cast<float>(bp_get(value, 8, 12)) / 255.f; // G
        }
        g_gxState.stateDirty = true;
      }
    } else {
      // TEV color register (11-bit signed components)
      if (idx < MaxTevRegs) {
        auto& cr = g_gxState.colorRegs[idx];
        if (isRA) {
          // 11-bit signed: sign-extend from 11 bits
          s32 r = bp_get(value, 11, 0);
          if (r & 0x400)
            r |= ~0x7FF; // sign extend
          s32 a = bp_get(value, 11, 12);
          if (a & 0x400)
            a |= ~0x7FF;
          cr[0] = static_cast<float>(r) / 255.f;
          cr[3] = static_cast<float>(a) / 255.f;
        } else {
          s32 b = bp_get(value, 11, 0);
          if (b & 0x400)
            b |= ~0x7FF;
          s32 g = bp_get(value, 11, 12);
          if (g & 0x400)
            g |= ~0x7FF;
          cr[2] = static_cast<float>(b) / 255.f;
          cr[1] = static_cast<float>(g) / 255.f;
        }
        g_gxState.stateDirty = true;
      }
    }
    break;
  }

  // Indirect texture matrices (0x06-0x0E), three consecutive registers per 3x2 matrix:
  // matrix 0 at 0x06, matrix 1 at 0x09, matrix 2 at 0x0C.
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x0E: {
    u32 idx = (regId - 0x06) / 3;    // matrix index (0-2)
    u32 column = (regId - 0x06) % 3; // column index (0-2)
    auto& info = g_gxState.indTexMtxs[idx];

    // Decode one packed matrix column: [m[0][column], m[1][column]].
    s32 col0 = bp_get(value, 11, 0);
    if (col0 & 0x400)
      col0 |= ~0x7FF; // sign-extend from 11 bits
    s32 col1 = bp_get(value, 11, 11);
    if (col1 & 0x400)
      col1 |= ~0x7FF;

    auto& packedColumn = column == 0 ? info.mtx.m0 : (column == 1 ? info.mtx.m1 : info.mtx.m2);
    packedColumn.x = static_cast<float>(col0) / 1024.0f;
    packedColumn.y = static_cast<float>(col1) / 1024.0f;

    // Accumulate the indirect matrix scale exponent. The SDK writes two bits per column, but the
    // hardware ignores the third column's top bit, leaving 5 bits for adjScale = scaleExp + 17.
    u32 scaleBits = bp_get(value, 2, 22);
    u32 shift = column * 2;
    if (column == 2) {
      info.adjScaleRaw = (info.adjScaleRaw & ~(1u << shift)) | ((scaleBits & 1u) << shift);
    } else {
      info.adjScaleRaw = (info.adjScaleRaw & ~(3u << shift)) | (scaleBits << shift);
    }
    info.scaleExp = static_cast<s8>(info.adjScaleRaw) - 17;

    g_gxState.stateDirty = true;
    break;
  }

  // SU texture coordinate scale registers (0x30-0x3F): even (suTs0) carry S-axis scale, bias, cyl
  // wrap and line/point offset; odd (suTs1) carry the T-axis equivalents.
  case 0x30:
  case 0x31:
  case 0x32:
  case 0x33:
  case 0x34:
  case 0x35:
  case 0x36:
  case 0x37:
  case 0x38:
  case 0x39:
  case 0x3A:
  case 0x3B:
  case 0x3C:
  case 0x3D:
  case 0x3E:
  case 0x3F: {
    u32 coordIdx = (regId - 0x30) / 2;
    bool isT = (regId & 1) != 0;
    auto& tcs = g_gxState.texCoordScales[coordIdx];
    if (isT) {
      tcs.scaleT = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasT = bp_get(value, 1, 16) != 0;
      tcs.cylWrapT = bp_get(value, 1, 17) != 0;
    } else {
      tcs.scaleS = static_cast<u16>(bp_get(value, 16, 0));
      tcs.biasS = bp_get(value, 1, 16) != 0;
      tcs.cylWrapS = bp_get(value, 1, 17) != 0;
      tcs.lineOffset = bp_get(value, 1, 18) != 0;
      tcs.pointOffset = bp_get(value, 1, 19) != 0;
    }
    g_gxState.stateDirty = true;
    break;
  }

  // Copy clear color (0x4F-0x50) and depth (0x51)
  case 0x49: {
    g_gxState.dispCopySrc.x = static_cast<int32_t>(bp_get(value, 10, 0));
    g_gxState.dispCopySrc.y = static_cast<int32_t>(bp_get(value, 10, 10));
    break;
  }
  case 0x4A: {
    g_gxState.dispCopySrc.width = static_cast<int32_t>(bp_get(value, 10, 0) + 1u);
    g_gxState.dispCopySrc.height = static_cast<int32_t>(bp_get(value, 10, 10) + 1u);
    break;
  }
  case 0x4D: {
    g_gxState.dispCopyDstWidth = static_cast<u16>(((bp_get(value, 10, 0) << 5) + 31u) >> 1);
    break;
  }
  case 0x4E: {
    const u32 iScale = bp_get(value, 9, 0);
    if (iScale != 0) {
      g_gxState.dispCopyYScale = 256.f / static_cast<float>(iScale);
    }
    break;
  }
  case 0x4F: {
    u8 r = bp_get(value, 8, 0);
    u8 a = bp_get(value, 8, 8);
    g_gxState.clearColor[0] = static_cast<float>(r) / 255.f;
    g_gxState.clearColor[3] = static_cast<float>(a) / 255.f;
    g_gxState.stateDirty = true;
    break;
  }
  case 0x50: {
    u8 b = bp_get(value, 8, 0);
    u8 g = bp_get(value, 8, 8);
    g_gxState.clearColor[2] = static_cast<float>(b) / 255.f;
    g_gxState.clearColor[1] = static_cast<float>(g) / 255.f;
    g_gxState.stateDirty = true;
    break;
  }
  case 0x51: {
    g_gxState.clearDepth = bp_get(value, 24, 0);
    g_gxState.stateDirty = true;
    break;
  }
  case 0xF4: {
    g_gxState.zTextureBias = value & 0x00FFFFFFu;
    mark_pipeline_state_dirty();
    break;
  }
  case 0xF5: {
    g_gxState.zTextureFmt = static_cast<u8>(bp_get(value, 2, 0));
    g_gxState.zTextureOp = static_cast<GXZTexOp>(bp_get(value, 2, 2));
    mark_pipeline_state_dirty();
    break;
  }
  case 0x52: {
    g_gxState.copyClamp = static_cast<GXFBClamp>(bp_get(value, 2, 0));
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(bp_get(value, 4, 3));
    g_gxState.dispCopyGamma = static_cast<GXGamma>(bp_get(value, 2, 7));
    g_gxState.texCopyHalfScale = bp_get(value, 1, 9) != 0;
    g_gxState.dispCopyFrame2Field = bp_get(value, 2, 12);
    break;
  }
  case 0x53: {
    g_gxState.copyFilterVFilter[0] = static_cast<u8>(bp_get(value, 6, 0));
    g_gxState.copyFilterVFilter[1] = static_cast<u8>(bp_get(value, 6, 6));
    g_gxState.copyFilterVFilter[2] = static_cast<u8>(bp_get(value, 6, 12));
    g_gxState.copyFilterVFilter[3] = static_cast<u8>(bp_get(value, 6, 18));
    refresh_copy_filter_flags();
    break;
  }
  case 0x54: {
    g_gxState.copyFilterVFilter[4] = static_cast<u8>(bp_get(value, 6, 0));
    g_gxState.copyFilterVFilter[5] = static_cast<u8>(bp_get(value, 6, 6));
    g_gxState.copyFilterVFilter[6] = static_cast<u8>(bp_get(value, 6, 12));
    refresh_copy_filter_flags();
    break;
  }
  case 0xE8:
  case 0xE9:
  case 0xEA:
  case 0xEB:
  case 0xEC:
  case 0xED:
    g_gxState.fogRange[regId - 0xE8] = value & 0x00FFFFFFu;
    mark_pipeline_state_dirty();
    break;

  default:
    if (const auto mapping = decode_tex_bp_reg(regId); mapping.has_value()) {
      auto& slot = g_gxState.loadedTextures[mapping->texMapId];
      bool changed = false;
      switch (mapping->kind) {
      case TexBpRegMapping::Kind::Mode0:
        changed = slot.mode0 != value;
        if (changed) {
          slot.mode0 = value;
        }
        break;
      case TexBpRegMapping::Kind::Mode1:
        changed = slot.mode1 != value;
        if (changed) {
          slot.mode1 = value;
        }
        break;
      case TexBpRegMapping::Kind::Image0:
        changed = slot.image0 != value;
        if (changed) {
          slot.image0 = value;
          slot.mWidth = 0;
          slot.mHeight = 0;
          slot.mFormat = gfx::InvalidTextureFormat;
        }
        break;
      case TexBpRegMapping::Kind::Image3:
        changed = slot.image3 != value;
        if (changed) {
          slot.image3 = value;
        }
        break;
      case TexBpRegMapping::Kind::Tlut:
        // TLUT region's TMEM offset
        break;
      case TexBpRegMapping::Kind::Image1:
      case TexBpRegMapping::Kind::Image2:
        // GXTexRegion regs
        break;
      }
      if (changed) {
        g_gxState.stateDirty = true;
      }
    } else {
#ifndef NDEBUG
      Log.debug("Unhandled BP register 0x{:02X} (value 0x{:06X})", regId, value & 0xFFFFFF);
#endif
    }
    break;
  }
}

extern "C" void GXApplyBPReg(u8 reg, u32 value) {
  handle_bp((static_cast<u32>(reg) << 24) | (value & 0x00FFFFFFu), true);
}

static bool cacheable_cp_register(u8 addr) {
  return addr == 0x30 || addr == 0x40 || addr == 0x50 || addr == 0x60 || (addr >= 0x70 && addr <= 0x97);
}

static std::array<u32, 0x100> s_cpRegisterCache{};
static std::array<bool, 0x100> s_cpRegisterCacheValid{};

void reset_cp_register_cache() {
  s_cpRegisterCache.fill(0);
  s_cpRegisterCacheValid.fill(false);
}

static bool cp_register_write_unchanged(u8 addr, u32 value) {
  if (!cacheable_cp_register(addr)) return false;

  if (s_cpRegisterCacheValid[addr] && s_cpRegisterCache[addr] == value) {
    return true;
  }
  s_cpRegisterCacheValid[addr] = true;
  s_cpRegisterCache[addr] = value;
  return false;
}

// CP register handler - decodes CP register writes and updates g_gxState
static void handle_cp(u8 addr, u32 value, bool bigEndian) {
  if (cp_register_write_unchanged(addr, value)) return;

  switch (addr) {
  // VCD low (0x50)
  case 0x50: {
    auto& vd = g_gxState.vtxDesc;
    auto& svd = g_gxState.sourceVtxDesc;
    vd[GX_VA_PNMTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 0));
    vd[GX_VA_TEX0MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 1));
    vd[GX_VA_TEX1MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 2));
    vd[GX_VA_TEX2MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 3));
    vd[GX_VA_TEX3MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 4));
    vd[GX_VA_TEX4MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 5));
    vd[GX_VA_TEX5MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 6));
    vd[GX_VA_TEX6MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 7));
    vd[GX_VA_TEX7MTXIDX] = static_cast<GXAttrType>(bp_get(value, 1, 8));
    vd[GX_VA_POS] = static_cast<GXAttrType>(bp_get(value, 2, 9));
    vd[GX_VA_NRM] = static_cast<GXAttrType>(bp_get(value, 2, 11));
    vd[GX_VA_CLR0] = static_cast<GXAttrType>(bp_get(value, 2, 13));
    vd[GX_VA_CLR1] = static_cast<GXAttrType>(bp_get(value, 2, 15));
    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_CLR1; ++attr) {
      svd[attr] = vd[attr];
    }
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    break;
  }

  // VCD high (0x60)
  case 0x60: {
    auto& vd = g_gxState.vtxDesc;
    auto& svd = g_gxState.sourceVtxDesc;
    vd[GX_VA_TEX0] = static_cast<GXAttrType>(bp_get(value, 2, 0));
    vd[GX_VA_TEX1] = static_cast<GXAttrType>(bp_get(value, 2, 2));
    vd[GX_VA_TEX2] = static_cast<GXAttrType>(bp_get(value, 2, 4));
    vd[GX_VA_TEX3] = static_cast<GXAttrType>(bp_get(value, 2, 6));
    vd[GX_VA_TEX4] = static_cast<GXAttrType>(bp_get(value, 2, 8));
    vd[GX_VA_TEX5] = static_cast<GXAttrType>(bp_get(value, 2, 10));
    vd[GX_VA_TEX6] = static_cast<GXAttrType>(bp_get(value, 2, 12));
    vd[GX_VA_TEX7] = static_cast<GXAttrType>(bp_get(value, 2, 14));
    for (int attr = GX_VA_TEX0; attr <= GX_VA_TEX7; ++attr) {
      svd[attr] = vd[attr];
    }
    mark_pipeline_state_dirty();
    g_gxState.clearVtxSizeCache();
    break;
  }

  // Matrix index A (0x30)
  case 0x30: {
    g_gxState.currentPnMtx = bp_get(value, 6, 0) / 3;
    for (u32 i = 0; i < 4 && i < MaxTexCoord; i++) {
      auto texMtx = static_cast<GXTexMtx>(bp_get(value, 6, 6 + i * 6));
      assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
      g_gxState.tcgs[i].mtx = texMtx;
    }
    // Same matrix indices as XF 0x18, written from a different bank.
    g_gxState.invalidateXfReg(0x18);
    mark_pipeline_state_dirty();
    break;
  }

  // Matrix index B (0x40)
  case 0x40: {
    for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
      auto texMtx = static_cast<GXTexMtx>(bp_get(value, 6, i * 6));
      assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
      g_gxState.tcgs[i + 4].mtx = texMtx;
    }
    // Same matrix indices as XF 0x19, written from a different bank.
    g_gxState.invalidateXfReg(0x19);
    mark_pipeline_state_dirty();
    break;
  }

  default:
    // VAT A registers (0x70-0x77)
    if (addr >= 0x70 && addr <= 0x77) {
      u32 fmt = addr - 0x70;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_POS].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_POS].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_POS].frac = static_cast<u8>(bp_get(value, 5, 4));
      vf.attrs[GX_VA_NRM].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      if (bp_get(value, 1, 31) != 0) {
        vf.attrs[GX_VA_NRM].cnt = GX_NRM_NBT3;
      } else {
        vf.attrs[GX_VA_NRM].cnt = bp_get(value, 1, 9) != 0 ? GX_NRM_NBT : GX_NRM_XYZ;
      }
      vf.attrs[GX_VA_NRM].frac = normal_frac_bits(vf.attrs[GX_VA_NRM].type);
      vf.attrs[GX_VA_CLR0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 13));
      vf.attrs[GX_VA_CLR0].type = static_cast<GXCompType>(bp_get(value, 3, 14));
      vf.attrs[GX_VA_CLR0].frac = 0;
      vf.attrs[GX_VA_CLR1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 17));
      vf.attrs[GX_VA_CLR1].type = static_cast<GXCompType>(bp_get(value, 3, 18));
      vf.attrs[GX_VA_CLR1].frac = 0;
      vf.attrs[GX_VA_TEX0].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 21));
      vf.attrs[GX_VA_TEX0].type = static_cast<GXCompType>(bp_get(value, 3, 22));
      vf.attrs[GX_VA_TEX0].frac = static_cast<u8>(bp_get(value, 5, 25));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // VAT B registers (0x80-0x87)
    else if (addr >= 0x80 && addr <= 0x87) {
      u32 fmt = addr - 0x80;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX1].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 0));
      vf.attrs[GX_VA_TEX1].type = static_cast<GXCompType>(bp_get(value, 3, 1));
      vf.attrs[GX_VA_TEX1].frac = static_cast<u8>(bp_get(value, 5, 4));
      vf.attrs[GX_VA_TEX2].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 9));
      vf.attrs[GX_VA_TEX2].type = static_cast<GXCompType>(bp_get(value, 3, 10));
      vf.attrs[GX_VA_TEX2].frac = static_cast<u8>(bp_get(value, 5, 13));
      vf.attrs[GX_VA_TEX3].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 18));
      vf.attrs[GX_VA_TEX3].type = static_cast<GXCompType>(bp_get(value, 3, 19));
      vf.attrs[GX_VA_TEX3].frac = static_cast<u8>(bp_get(value, 5, 22));
      vf.attrs[GX_VA_TEX4].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 27));
      vf.attrs[GX_VA_TEX4].type = static_cast<GXCompType>(bp_get(value, 3, 28));
      // TEX4 frac is in VAT C
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // VAT C registers (0x90-0x97)
    else if (addr >= 0x90 && addr <= 0x97) {
      u32 fmt = addr - 0x90;
      auto& vf = g_gxState.vtxFmts[fmt];
      vf.attrs[GX_VA_TEX4].frac = static_cast<u8>(bp_get(value, 5, 0));
      vf.attrs[GX_VA_TEX5].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 5));
      vf.attrs[GX_VA_TEX5].type = static_cast<GXCompType>(bp_get(value, 3, 6));
      vf.attrs[GX_VA_TEX5].frac = static_cast<u8>(bp_get(value, 5, 9));
      vf.attrs[GX_VA_TEX6].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 14));
      vf.attrs[GX_VA_TEX6].type = static_cast<GXCompType>(bp_get(value, 3, 15));
      vf.attrs[GX_VA_TEX6].frac = static_cast<u8>(bp_get(value, 5, 18));
      vf.attrs[GX_VA_TEX7].cnt = static_cast<GXCompCnt>(bp_get(value, 1, 23));
      vf.attrs[GX_VA_TEX7].type = static_cast<GXCompType>(bp_get(value, 3, 24));
      vf.attrs[GX_VA_TEX7].frac = static_cast<u8>(bp_get(value, 5, 27));
      mark_pipeline_state_dirty();
      g_gxState.clearVtxSizeCache();
    }
    // Array base addresses (0xA0-0xAF)
    else if (addr >= 0xA0 && addr <= 0xAF) {
      Log.error("CP_REG_ARRAYBASE_ID is not supported on Aurora. Use GX_LOAD_AURORA_ARRAYBASE instead.");
    }
    // Array strides (0xB0-0xBF)
    else if (addr >= 0xB0 && addr <= 0xBF) {
      u32 attrIdx = addr - 0xB0 + GX_VA_POS;
      if (attrIdx < GX_VA_MAX_ATTR) {
        auto& array = g_gxState.arrays[attrIdx];
        const auto newStride = static_cast<u8>(value);
        if (array.stride != newStride) {
          array.stride = newStride;
          mark_pipeline_state_dirty();
        }
      }
    }
    break;
  }
}

// XF register handler - decodes XF (transform unit) register writes and updates g_gxState
static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {
  // These bounds must hold in release too: CHECK() is a no-op under NDEBUG, so relying on it alone let a truncated guest display list read past `data`.
  if (pos > size || size - pos < 4) UNLIKELY {
      CHECK(false, "XF header read overrun");
      pos = size;
      return;
    }
  u32 header = read_u32(data + pos, bigEndian);
  pos += 4;

  u32 count = ((header >> 16) & 0xFFFF) + 1;
  u32 addr = header & 0xFFFF;
  u32 dataBytes = count * 4;
  // Log.warn("  xf: addr {:04x} count {} dataBytes {} pos {} -> {}", addr, count, dataBytes, pos, pos + dataBytes);
  if (size - pos < dataBytes) UNLIKELY {
      CHECK(false, "XF data read overrun: need {} bytes at pos {}", dataBytes, pos);
      pos = size;
      return;
    }

  const u8* xfData = data + pos;

  if (copy_xf_data(addr, xfData, count, bigEndian)) {
    // copy_xf_data handled everything.
  } else if (addr >= 0x1000) {
    // XF registers (0x1000+)
    u32 xfAddr = addr - 0x1000;
    bool viewportUpdated = false;
    bool projectionUpdated = false;
    for (u32 i = 0; i < count; i++) {
      u32 reg = xfAddr + i;
      u32 val = read_u32(xfData + i * 4, bigEndian);

      // Skip register writes that decode to state we already hold.
      const bool cacheable = reg < g_gxState.xfRegCache.size();
      const bool unchanged = cacheable && g_gxState.xfRegMatches(reg, val);
      if (cacheable) g_gxState.storeXfReg(reg, val);
      // Viewport (0x1A-0x1F) and projection (0x20-0x26) keep their unconditional apply below; only the banks that already had skip semantics and the TexGen bank drop out here.
      if (unchanged && (reg <= 0x19 || reg >= 0x3F)) continue;

      switch (reg) {
      case 0x00:
        g_gxState.xfError = val;
        break;
      case 0x08:
        // XF vertex specs (numColors, numNormals, numTexCoords) - informational
        break;
      case 0x09:
        // numChans
        g_gxState.numChans = val;
        g_gxState.stateDirty = true;
        break;
      case 0x0A:
        // Ambient color 0
        g_gxState.colorChannelState[GX_COLOR0].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0B:
        // Ambient color 1
        g_gxState.colorChannelState[GX_COLOR1].ambColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].ambColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0C:
        // Material color 0
        g_gxState.colorChannelState[GX_COLOR0].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA0].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0D:
        // Material color 1
        g_gxState.colorChannelState[GX_COLOR1].matColor = unpack_color(val);
        g_gxState.colorChannelState[GX_ALPHA1].matColor = unpack_color(val);
        g_gxState.stateDirty = true;
        break;
      case 0x0E:
      case 0x0F:
      case 0x10:
      case 0x11: {
        // Channel control registers
        u32 chanId = reg - 0x0E;
        if (chanId < MaxColorChannels) {
          auto& chan = g_gxState.colorChannelConfig[chanId];
          chan.matSrc = static_cast<GXColorSrc>(bp_get(val, 1, 0));
          chan.lightingEnabled = bp_get(val, 1, 1) != 0;
          u32 lightsLo = bp_get(val, 4, 2);
          chan.ambSrc = static_cast<GXColorSrc>(bp_get(val, 1, 6));
          chan.diffFn = static_cast<GXDiffuseFn>(bp_get(val, 2, 7));
          u32 lightsHi = bp_get(val, 4, 11);
          switch (bp_get(val, 2, 9)) {
          case 1:
            chan.attnFn = GX_AF_SPEC;
            break;
          case 3:
            chan.attnFn = GX_AF_SPOT;
            break;
          case 0:
          case 2:
          default:
            chan.attnFn = GX_AF_NONE;
            break;
          }
          u32 lightMask = lightsLo | (lightsHi << 4);
          g_gxState.colorChannelState[chanId].lightMask = GX::LightMask{lightMask};
          mark_pipeline_state_dirty();
        }
        break;
      }
      case 0x12:
        g_gxState.dualTex = val;
        mark_pipeline_state_dirty();
        break;
      case 0x18: {
        // Matrix index A: PnMtx + TexCoord0-3 matrix indices
        g_gxState.currentPnMtx = bp_get(val, 6, 0) / 3;
        for (u32 i = 0; i < 4 && i < MaxTexCoord; i++) {
          auto texMtx = static_cast<GXTexMtx>(bp_get(val, 6, 6 + i * 6));
          assert(texMtx >= 0 && texMtx <= GXTexMtx::GX_IDENTITY);
          g_gxState.tcgs[i].mtx = texMtx;
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x19: {
        // Matrix index B: TexCoord4-7 matrix indices
        for (u32 i = 0; i < 4 && (i + 4) < MaxTexCoord; i++) {
          g_gxState.tcgs[i + 4].mtx = static_cast<GXTexMtx>(bp_get(val, 6, i * 6));
        }
        mark_pipeline_state_dirty();
        break;
      }
      case 0x1A:
      case 0x1B:
      case 0x1C:
      case 0x1D:
      case 0x1E:
      case 0x1F: {
        // Viewport: sx, sy, sz, ox, oy, oz at XF 0x101A-0x101F
        const u32 vpOff = reg - 0x1A;
        g_gxState.xfViewport[vpOff] = read_f32(xfData + i * 4, bigEndian);
        viewportUpdated = true;
        break;
      }
      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23:
      case 0x24:
      case 0x25:
      case 0x26: {
        // Projection: 6 params + type at XF 0x1020-0x1026
        const u32 projOff = reg - 0x20;
        if (projOff < g_gxState.xfProjection.size()) {
          g_gxState.xfProjection[projOff] = read_f32(xfData + i * 4, bigEndian);
        } else {
          g_gxState.projType = static_cast<GXProjectionType>(val);
        }
        projectionUpdated = true;
        break;
      }
      case 0x3F:
        // numTexGens
        g_gxState.numTexGens = val;
        mark_pipeline_state_dirty();
        break;
      default:
        // TexGen config (0x40-0x4F) and post-transform (0x50-0x5F)
        if (reg >= 0x40 && reg <= 0x4F) {
          u32 tcIdx = reg - 0x40;
          if (tcIdx < MaxTexCoord) {
            auto& tcg = g_gxState.tcgs[tcIdx];
            bool proj = bp_get(val, 1, 1) != 0;
            u32 form = bp_get(val, 1, 2);
            u32 tgType = bp_get(val, 3, 4);
            u32 srcRow = bp_get(val, 5, 7);
            tcg.inputFormAB11 = form == 0;

            if (tgType == 0) {
              tcg.type = proj ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            } else if (tgType == 1) {
              // Bump mapping
              tcg.type = static_cast<GXTexGenType>(bp_get(val, 3, 15) + 2);
            } else if (tgType == 2 || tgType == 3) {
              tcg.type = GX_TG_SRTG;
            }

            // Decode source from row
            static const GXTexGenSrc rowToSrc[] = {GX_TG_POS,  GX_TG_NRM,  GX_TG_COLOR0, GX_TG_BINRM, GX_TG_TANGENT,
                                                   GX_TG_TEX0, GX_TG_TEX1, GX_TG_TEX2,   GX_TG_TEX3,  GX_TG_TEX4,
                                                   GX_TG_TEX5, GX_TG_TEX6, GX_TG_TEX7};
            if (srcRow < 13) {
              tcg.src = rowToSrc[srcRow];
            }
            mark_pipeline_state_dirty();
          }
        } else if (reg >= 0x50 && reg <= 0x5F) {
          u32 tcIdx = reg - 0x50;
          if (tcIdx < MaxTexCoord) {
            g_gxState.tcgs[tcIdx].postMtx = static_cast<GXPTTexMtx>(bp_get(val, 6, 0) + 64);
            g_gxState.tcgs[tcIdx].normalize = bp_get(val, 1, 8) != 0;
            mark_pipeline_state_dirty();
          }
        } else {
#ifndef NDEBUG
          Log.debug("Unhandled XF register 0x{:04X} (value 0x{:08X})", reg, val);
#endif
        }
        break;
      }
    }
    if (viewportUpdated) {
      apply_xf_viewport();
    }
    if (projectionUpdated) {
      apply_xf_projection();
    }
  }

  pos += dataBytes;
}

static void handle_draw_overrun(u8 cmd, u16 vtxCount, u32 vtxSize, u32 totalVtxBytes, const u8* data, const u32& pos,
                                u32 size) {
  static u32 truncatedDrawLogCount = 0;
  if (truncatedDrawLogCount >= 64) {
    if (truncatedDrawLogCount == 64) {
      Log.warn("suppressing further truncated draw diagnostics");
    }
    ++truncatedDrawLogCount;
    return;
  }
  ++truncatedDrawLogCount;

  // Hex dump around the draw command for debugging
  u32 cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  u32 dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  u32 dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (u32 i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.warn("  hex dump around truncated draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  Log.warn("  truncated draw cmd=0x{:02X} fmt={} vtxCount={} vtxSize={} desc pn={} pos={} nrm={} clr0={} clr1={} "
            "tex0={} tex1={} tex2={} tex3={} tex4={} tex5={} tex6={} tex7={}",
            cmd, static_cast<u32>(fmt), vtxCount, vtxSize, g_gxState.vtxDesc[GX_VA_PNMTXIDX],
            g_gxState.vtxDesc[GX_VA_POS], g_gxState.vtxDesc[GX_VA_NRM], g_gxState.vtxDesc[GX_VA_CLR0],
            g_gxState.vtxDesc[GX_VA_CLR1], g_gxState.vtxDesc[GX_VA_TEX0], g_gxState.vtxDesc[GX_VA_TEX1],
            g_gxState.vtxDesc[GX_VA_TEX2], g_gxState.vtxDesc[GX_VA_TEX3], g_gxState.vtxDesc[GX_VA_TEX4],
            g_gxState.vtxDesc[GX_VA_TEX5], g_gxState.vtxDesc[GX_VA_TEX6], g_gxState.vtxDesc[GX_VA_TEX7]);
  Log.warn("  fmt {} attrs pos({},{}) nrm({},{}) clr0({},{}) tex0({},{}) tex1({},{})",
            static_cast<u32>(fmt), static_cast<u32>(vtxFmt.attrs[GX_VA_POS].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_POS].type), static_cast<u32>(vtxFmt.attrs[GX_VA_NRM].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_NRM].type), static_cast<u32>(vtxFmt.attrs[GX_VA_CLR0].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_CLR0].type), static_cast<u32>(vtxFmt.attrs[GX_VA_TEX0].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_TEX0].type), static_cast<u32>(vtxFmt.attrs[GX_VA_TEX1].cnt),
            static_cast<u32>(vtxFmt.attrs[GX_VA_TEX1].type));
  Log.warn("stopping FIFO decode at truncated draw: need {} bytes at pos {}, have {}", totalVtxBytes, pos, size);
}

// Draw command handler - parses vertices inline and caches results
static u32 calculate_last_vtx_size(GXVtxFmt fmt) {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += (attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3) ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += (attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3) ? 6 : 2;
      break;
    }
  }

  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  // The format is selected by the draw opcode, without a register write.
  // Even equal-stride formats may decode bytes differently, so do not merge
  // into a draw using the previous format's shader and uniform layout.
  g_gxState.stateDirty = true;

  return vtxSize;
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount,
                                 gfx::Range vertRange, uint16_t usedPnMtxMask,
                                 HashType matrixTopologySignature,
                                 HashType geometrySignature, bool interpolationIdentityActive);

// The per-draw geometry signature, matrix-usage mask and draw-identity hashes exist purely to feed frame interpolation (build_uniform consumes them only after its `frame_interpolation_fps() == 0` early-out).
static inline bool frame_interpolation_identity_needed() noexcept {
  return frame_interpolation_active() && g_gxState.projType == GX_PERSPECTIVE;
}

static uint32_t matrix_index_prefix_size(GXVtxFmt fmt) noexcept {
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  uint32_t size = 0;
  for (int i = GX_VA_PNMTXIDX; i < GX_VA_POS; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT:
      size += comp_type_size(attr, vtxFmt.attrs[i].type) *
              comp_cnt_count(attr, vtxFmt.attrs[i].cnt);
      break;
    case GX_INDEX8:
      ++size;
      break;
    case GX_INDEX16:
      size += 2;
      break;
    }
  }
  return size;
}

static HashType draw_geometry_signature(GXVtxFmt fmt, const uint8_t* vertices,
                                        uint16_t vtxCount, uint32_t vtxStride) noexcept {
  Hasher hasher;
  hasher.update(vtxCount);
  hasher.update(vtxStride);

  // Matrix-index bytes select an instance's current XF palette slots, so they are deliberately excluded from mesh identity.
  const uint32_t matrixPrefix = std::min(matrix_index_prefix_size(fmt), vtxStride);
  if (matrixPrefix == 0) {
    // Most draws have no direct matrix-index prefix.
    hasher.update(vertices, static_cast<size_t>(vtxCount) * vtxStride);
  } else {
    for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
      hasher.update(vertices + static_cast<size_t>(vertex) * vtxStride + matrixPrefix,
                    vtxStride - matrixPrefix);
    }
  }

  // Identical index streams can address different vertex arrays.
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    const auto& array = g_gxState.arrays[i];
    const auto attribute = static_cast<uint32_t>(i);
    const auto source = reinterpret_cast<uintptr_t>(array.data);
    hasher.update(attribute);
    hasher.update(source);
    hasher.update(array.stride);
  }
  return static_cast<HashType>(hasher.digest());
}

struct PnMtxUsage {
  uint16_t mask = 0;
  HashType topologySignature = 0;
};

// Slot mask only.
static uint16_t pn_mtx_mask(const uint8_t* vertices, uint16_t vtxCount,
                            uint32_t vtxStride) noexcept {
  if (g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_DIRECT) {
    return static_cast<uint16_t>(1u << std::min<uint32_t>(g_gxState.currentPnMtx, MaxPnMtx - 1));
  }
  uint16_t mask = 0;
  for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
    const uint32_t matrixIndex = vertices[static_cast<size_t>(vertex) * vtxStride] / 3u;
    if (matrixIndex < MaxPnMtx) {
      mask |= static_cast<uint16_t>(1u << matrixIndex);
    }
  }
  return mask;
}

static PnMtxUsage pn_mtx_usage(const uint8_t* vertices, uint16_t vtxCount,
                               uint32_t vtxStride) noexcept {
  if (g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_DIRECT) {
    const uint32_t matrixIndex = std::min<uint32_t>(g_gxState.currentPnMtx, MaxPnMtx - 1);
    return {
        .mask = static_cast<uint16_t>(1u << matrixIndex),
        // There is no vertex matrix-index topology in this path.
        .topologySignature = 0,
    };
  }

  Hasher topologyHasher;
  topologyHasher.update(vtxCount);
  uint16_t mask = 0;
  for (uint16_t vertex = 0; vertex < vtxCount; ++vertex) {
    // GX matrix-index attributes precede every other vertex attribute and are always one byte.
    const uint8_t rawMatrixIndex = vertices[static_cast<size_t>(vertex) * vtxStride];
    topologyHasher.update(rawMatrixIndex);
    const uint32_t matrixIndex = rawMatrixIndex / 3u;
    if (matrixIndex < MaxPnMtx) {
      mask |= static_cast<uint16_t>(1u << matrixIndex);
    }
  }
  return {
      .mask = mask,
      .topologySignature = static_cast<HashType>(topologyHasher.digest()),
  };
}

// Whether the most recent unmerged draw recorded an interpolation snapshot, so a draw merged into it knows there is a snapshot to extend.
static bool s_lastDrawRecordedInterpolation = false;

struct CachedIndexTemplate {
  bool valid = false;
  GXPrimitive prim = static_cast<GXPrimitive>(0);
  u16 vtxCount = 0;
  u32 indexCount = 0;
  IndexBuffer indices{};
};

static const CachedIndexTemplate& cached_index_template(GXPrimitive prim, u16 vtxCount) {
  // Topology expansion is immutable for a primitive/count pair.
  constexpr size_t CacheSize = 256;
  static std::array<CachedIndexTemplate, CacheSize> cache{};
  const u32 key = (static_cast<u32>(underlying(prim)) << 16) | vtxCount;
  auto& entry = cache[(key ^ (key >> 9)) & (CacheSize - 1)];
  if (entry.valid && entry.prim == prim && entry.vtxCount == vtxCount) LIKELY {
    return entry;
  }

  entry.valid = true;
  entry.prim = prim;
  entry.vtxCount = vtxCount;
  entry.indexCount = prepare_idx_template(entry.indices, prim, vtxCount);
  return entry;
}

static IndexBuffer handle_draw_idx_buf;

static ArrayRef<u16> offset_index_template(const CachedIndexTemplate& indexTemplate,
                                           u16 vtxStart) {
  // Grow-only: resizing down and back up made every merge zero-fill the buffer before the transform immediately overwrote it.
  const size_t count = indexTemplate.indices.size();
  if (handle_draw_idx_buf.size() < count) {
    handle_draw_idx_buf.resize(count);
  }
  const u16* src = indexTemplate.indices.data();
  u16* dst = handle_draw_idx_buf.data();
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<u16>(src[i] + vtxStart);
  }
  return {dst, count};
}

struct CachedPipelineState {
  gfx::PipelineRef ref = 0;
  HashType configHash = 0;
  // Carried here so the draw can be recorded without keeping the PipelineConfig that produced it alive; it is the only field of the config the draw itself still needs.
  u32 dstAlpha = UINT32_MAX;
  ShaderInfo shaderInfo{};
};

static const CachedPipelineState& cached_pipeline_state(const PipelineConfig& config) {
  constexpr size_t CacheSize = 1024;
  struct Entry {
    bool valid = false;
    PipelineConfig config{};
    CachedPipelineState state{};
  };
  static std::array<Entry, CacheSize> cache{};

  const HashType hash = xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX));
  auto& entry = cache[hash & (CacheSize - 1)];
  if (entry.valid && entry.state.configHash == hash &&
      std::memcmp(&entry.config, &config, sizeof(config)) == 0) LIKELY {
    return entry.state;
  }

  entry.valid = true;
  entry.config = config;
  entry.state = {
      .ref = gfx::pipeline_ref(config),
      .configHash = hash,
      .dstAlpha = config.dstAlpha,
      .shaderInfo = build_shader_info(config.shaderConfig),
  };
  return entry.state;
}

// Resolving a pipeline the long way costs a ~2.7KB zero-init, a full populate_pipeline_config, an XXH3 over the whole config and a memcmp against the hash-indexed slot -- roughly 11KB of memory traffic for a result that is almost always identical to the previous draw's.
static const CachedPipelineState& resolve_pipeline_state(GXPrimitive prim, GXVtxFmt fmt) {
  struct Memo {
    const CachedPipelineState* state = nullptr;
    u32 generation = 0;
    u32 sampleCount = 0;
    GXPrimitive prim = static_cast<GXPrimitive>(0);
    GXVtxFmt fmt = static_cast<GXVtxFmt>(0);
  };
  static Memo memo{};

  const u32 sampleCount = gfx::get_sample_count();
  const u32 generation = g_gxState.pipelineStateGeneration;
  if (memo.state != nullptr && memo.generation == generation && memo.sampleCount == sampleCount &&
      memo.prim == prim && memo.fmt == fmt) LIKELY {
    return *memo.state;
  }

  PipelineConfig config{};
  populate_pipeline_config(config, prim, fmt);
  // cached_pipeline_state hands back a reference into a fixed direct-mapped table, so the address stays valid; the entry it points at can only be rewritten by another call to that function, and every such call goes through this miss path and replaces the memo in the same breath.
  const CachedPipelineState& state = cached_pipeline_state(config);
  memo = Memo{
      .state = &state,
      .generation = generation,
      .sampleCount = sampleCount,
      .prim = prim,
      .fmt = fmt,
  };
  return state;
}

static bool admit_draw(GXPrimitive prim, GXVtxFmt fmt, u16 count, uint32_t vertexBytes, bool merged = false) {
  const auto& indexTemplate = cached_index_template(prim, count);
  gfx::StagingSizes demand{vertexBytes, 0, indexTemplate.indices.size() * sizeof(u16), 0};
  if (merged) return gfx::staging_has_space(demand);
  const auto& info = resolve_pipeline_state(prim, fmt).shaderInfo;
  demand[1] = gfx::staging_uniform_bytes(info.uniformSize);
  if (frame_interpolation_identity_needed() && frame_interpolation_replay_safe())
    demand[1] *= 1 + MaxInterpolatedFrames;
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if ((g_gxState.vtxDesc[i] == GX_INDEX8 || g_gxState.vtxDesc[i] == GX_INDEX16) &&
        g_gxState.arrays[i].cachedRange.size == 0)
      demand[3] += gfx::staging_storage_bytes(g_gxState.arrays[i].size);
  }
  return gfx::staging_has_space(demand);
}

bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes) {
  ZoneScoped;
  if (vertices == nullptr || vtxCount == 0 || vertexBytes == 0) {
    return false;
  }

  if (__gx->dirtyState != 0) UNLIKELY {
    __GXSetDirtyState();
  }

  // Raw bridge draws consume live decoded GX state that is also maintained by the HLE producer.
  drain();

  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt) LIKELY {
    vtxSize = g_gxState.lastVtxSize;
  } else UNLIKELY {
    vtxSize = calculate_last_vtx_size(fmt);
  }

  const u32 expectedVertexBytes = static_cast<u32>(vtxCount) * vtxSize;
  if (expectedVertexBytes == 0 || expectedVertexBytes != vertexBytes) {
    static uint32_t rawVertexSizeMismatchCount = 0;
    if (rawVertexSizeMismatchCount++ < 64) {
      Log.warn("raw draw vertex-size mismatch prim={} fmt={} count={} cached_stride={} expected={} supplied={}",
               static_cast<uint32_t>(prim), static_cast<uint32_t>(fmt), vtxCount, vtxSize,
               expectedVertexBytes, vertexBytes);
    }
    return false;
  }

  if (!has_complete_primitive(prim, vtxCount)) return true;

  // This entry point bypasses process(), so it owns the renderer lock itself.
  std::unique_lock gpuLock(aurora::renderer_gpu_mutex());
  if (!admit_draw(prim, fmt, vtxCount, vertexBytes)) {
    gpuLock.unlock();
    gfx::split_staging_batch();
    gpuLock.lock();
    if (!admit_draw(prim, fmt, vtxCount, vertexBytes))
      throw gfx::StagingCapacityError("Raw draw does not fit after capacity submission");
  }
  const gfx::Range vertRange = gfx::push_verts(vertices, vertexBytes);
  const bool interpolationIdentityActive = frame_interpolation_identity_needed();
  const PnMtxUsage matrixUsage = interpolationIdentityActive
                                     ? pn_mtx_usage(vertices, vtxCount, vtxSize)
                                     : PnMtxUsage{};
  handle_draw_unmerged(prim, fmt, vtxCount, vertRange,
                       matrixUsage.mask, matrixUsage.topologySignature,
                       interpolationIdentityActive ? draw_geometry_signature(fmt, vertices, vtxCount, vtxSize) : 0,
                       interpolationIdentityActive);
  return true;
}

static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {
  ZoneScoped;
  GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  GXPrimitive prim = primitive_from_draw_cmd(cmd);

  if (pos + 2 > size) {
    return false;
  }
  u16 vtxCount = read_u16(data + pos, bigEndian);
  pos += 2;

  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt) LIKELY {
    vtxSize = g_gxState.lastVtxSize;
  } else UNLIKELY {
    vtxSize = calculate_last_vtx_size(fmt);
  }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (pos + totalVtxBytes > size) UNLIKELY {
    handle_draw_overrun(cmd, vtxCount, vtxSize, totalVtxBytes, data, pos, size);
    return false;
  }


  if (!has_complete_primitive(prim, vtxCount)) {
    pos += totalVtxBytes;
    return true;
  }

  DrawData* mergeTarget = nullptr;
  // Decide admission before allocating anything. The merged path needs only
  // vertices and indices; it must not resolve pipelines or upload arrays.
  // Try to merge with previous draw call
  if (!g_gxState.stateDirty) LIKELY {
    auto* lastDraw = gfx::get_last_draw_command<DrawData>();
    // Expanded lines/points have different vertex interpretation even with one instance.
    // Triangle-list output has no restart index; index 65535 is usable.
    // Overflow would address earlier vertices instead of the appended geometry.
    if (lastDraw != nullptr && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS &&
        !lastDraw->expandedPrimitive && lastDraw->instanceCount == 1 &&
        uint64_t(lastDraw->vtxCount) +
            vtxCount <= 65536u) LIKELY {
      mergeTarget = lastDraw;
    }
  }
  if (!admit_draw(prim, fmt, vtxCount, totalVtxBytes, mergeTarget != nullptr)) throw gfx::StagingBatchFull{};
  const uint8_t* vertices = data + pos;
  gfx::Range vertRange = gfx::push_verts(vertices, totalVtxBytes);
  pos += totalVtxBytes;
  if (auto* lastDraw = mergeTarget) {
      const auto& indexTemplate = cached_index_template(prim, vtxCount);
      const auto indices = offset_index_template(indexTemplate, lastDraw->vtxCount);
      const u32 numIndices = indexTemplate.indexCount;
      const gfx::Range idxRange = gfx::push_indices(indices);
      CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
            "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
            vertRange.offset);
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
      lastDraw->vertRange.size += vertRange.size;
      lastDraw->idxRange.size += idxRange.size;
      lastDraw->vtxCount += vtxCount;
      lastDraw->indexCount += numIndices;
      ++gfx::g_mergedDrawCallCount;
      // This primitive now renders through the draw we merged into, so its palette slots belong to that draw's interpolation snapshot as well.
      if (s_lastDrawRecordedInterpolation) UNLIKELY {
        extend_interpolation_draw(pn_mtx_mask(vertices, vtxCount, vtxSize));
      }
      return true;
  }

  const bool interpolationIdentityActive = frame_interpolation_identity_needed();
  const PnMtxUsage matrixUsage = interpolationIdentityActive
                                     ? pn_mtx_usage(vertices, vtxCount, vtxSize)
                                     : PnMtxUsage{};
  handle_draw_unmerged(prim, fmt, vtxCount, vertRange,
                       matrixUsage.mask, matrixUsage.topologySignature,
                       interpolationIdentityActive ? draw_geometry_signature(fmt, vertices, vtxCount, vtxSize) : 0,
                       interpolationIdentityActive);
  return true;
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount,
                                 gfx::Range vertRange, uint16_t usedPnMtxMask,
                                 HashType matrixTopologySignature,
                                 HashType geometrySignature, bool interpolationIdentityActive) {
  ZoneScoped;
  // GX_CULL_ALL rasterizes nothing on hardware - no color, no depth.
  if (g_gxState.cullMode == GX_CULL_ALL && prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS)
      UNLIKELY {
    // Leave stateDirty alone: the next draw re-resolving its pipeline is the safe direction, and nothing about this draw reached the GPU.
    return;
  }
  // Callers hold the renderer GPU mutex for the whole drain (process() and submit_raw_draw); taking it again per draw only cost a recursive re-entry.
  const auto& indexTemplate = cached_index_template(prim, vtxCount);
  const u32 numIndices = indexTemplate.indexCount;
  const gfx::Range idxRange = gfx::push_indices(ArrayRef<u16>{
      indexTemplate.indices.data(), indexTemplate.indices.size()});

  // Build pipeline, bind groups, and push draw command
  BindGroupRanges ranges{};
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (g_gxState.vtxDesc[i] != GX_INDEX8 && g_gxState.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = g_gxState.arrays[i];
    if (array.cachedRange.size > 0) {
      ranges.vaRanges[i - GX_VA_POS] = array.cachedRange;
    } else {
      const auto range = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
      ranges.vaRanges[i - GX_VA_POS] = range;
      array.cachedRange = range;
    }
  }

  const auto& pipelineState = resolve_pipeline_state(prim, fmt);
  const auto& info = pipelineState.shaderInfo;

  resolve_sampled_textures(info);

  const auto bindGroups = build_bind_groups(info);

  const auto pipeline = pipelineState.ref;

  // Draw-identity hashing only feeds frame interpolation, and only for perspective draws: build_uniform reads the identity exclusively past its `!perspective || frame_interpolation_fps() == 0` early-out.
  FrameInterpolationDrawIdentity drawIdentity{};
  if (interpolationIdentityActive) UNLIKELY {
    const HashType drawShape = static_cast<HashType>(vtxCount) |
                               (static_cast<HashType>(underlying(prim)) << 16) |
                               (static_cast<HashType>(underlying(fmt)) << 24);
    const HashType pipelineDrawSignature = xxh3_hash(pipelineState.configHash, drawShape);
    const HashType textureSignature = xxh3_hash(bindGroups.textureBindGroup);
    const HashType materialAndTopology =
        xxh3_hash(matrixTopologySignature,
                  xxh3_hash(bindGroups.textureBindGroup, pipelineDrawSignature));
    drawIdentity = FrameInterpolationDrawIdentity{
        .combined = xxh3_hash(geometrySignature, materialAndTopology),
        .pipeline = pipelineDrawSignature,
        .texture = textureSignature,
        .matrixTopology = matrixTopologySignature,
    };
  }
  const auto uniformRanges =
      build_uniform(info, vertRange.offset, ranges, drawIdentity, interpolationIdentityActive,
                    usedPnMtxMask);
  s_lastDrawRecordedInterpolation = interpolationIdentityActive;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  gfx::push_draw_command(DrawData{
      .pipeline = pipeline,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = uniformRanges.current,
      .interpolatedUniformRanges = uniformRanges.interpolated,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .expandedPrimitive = prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS,
      .bindGroups = bindGroups,
      .dstAlpha = pipelineState.dstAlpha,
  });
  g_gxState.stateDirty = false;
}

std::string read_string(const u8* data, u32& pos, u32 size, bool bigEndian) {
  CHECK(pos + 2 <= size, "Aurora string length read overrun");
  const u16 length = read_u16(data + pos, bigEndian);
  pos += 2;

  CHECK(pos + length <= size, "Aurora string read overrun");
  std::string str(reinterpret_cast<const char*>(data) + pos, length);
  pos += length;
  return str;
}

bool handle_aurora(const u8* data, u32& pos, u32 size, bool bigEndian) {
  ZoneScoped;
  if (pos + 2 > size) {
    return false;
  }
  u16 subCmd = read_u16(data + pos, bigEndian);
  pos += 2;

  // Setting of vertex array bases.
  if (subCmd == GX_LOAD_AURORA_VIEWPORT_RENDER) {
    CHECK(pos + 24 <= size, "GX_LOAD_AURORA_VIEWPORT_RENDER read overrun");
    const f32 left = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 top = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 width = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 height = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 nearZ = read_f32(data + pos, bigEndian);
    pos += 4;
    const f32 farZ = read_f32(data + pos, bigEndian);
    pos += 4;
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_LOAD_AURORA_SCISSOR_RENDER) {
    CHECK(pos + 16 <= size, "GX_LOAD_AURORA_SCISSOR_RENDER read overrun");
    const int32_t left = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t top = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t width = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    const int32_t height = static_cast<int32_t>(read_u32(data + pos, bigEndian));
    pos += 4;
    set_render_scissor({left, top, width, height});
  } else if (subCmd >= GX_LOAD_AURORA_ARRAYBASE && subCmd <= (GX_LOAD_AURORA_ARRAYBASE | 0x0f)) {
    CHECK(pos + 13 <= size, "GX_LOAD_AURORA_ARRAYBASE read overrun");
    u32 attrIdx = subCmd - GX_LOAD_AURORA_ARRAYBASE + GX_VA_POS;

    u64 arrayAddr = read_u64(data + pos, bigEndian);
    pos += 8;
    u32 arraySize = read_u32(data + pos, bigEndian);
    pos += 4;
    bool le = data[pos] == 1;
    pos += 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      // Only drop the cached upload when the backing array actually changes.
      array.cachedRange = {};
      mark_pipeline_state_dirty();
    }
  } else if (subCmd == GX_LOAD_AURORA_TEXOBJ) {
    CHECK(pos + 34 <= size, "GX_LOAD_AURORA_TEXOBJ read overrun");
    const auto texMapId = data[pos];
    pos += 1;
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    GXTexObj_ next = slot;
    next.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    next.mWidth = read_u32(data + pos, bigEndian);
    pos += 4;
    next.mHeight = read_u32(data + pos, bigEndian);
    pos += 4;
    next.mFormat = static_cast<GXTexFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    next.tlut = static_cast<GXTlut>(read_u32(data + pos, bigEndian));
    pos += 4;
    if (data[pos] != 0) {
      next.flags |= 1u;
    } else {
      next.flags &= ~1u;
    }
    pos += 1;
    next.texObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    next.texDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    next.set_no_cache(false); // Reset no-cache flag
    const bool changed = slot.data != next.data || slot.mWidth != next.mWidth || slot.mHeight != next.mHeight ||
                         slot.mFormat != next.mFormat || slot.tlut != next.tlut || slot.flags != next.flags ||
                         slot.texObjId != next.texObjId || slot.texDataVersion != next.texDataVersion;
    slot = next;
    if (changed) {
      g_gxState.stateDirty = true;
    }
  } else if (subCmd == GX_LOAD_AURORA_TLUT) {
    CHECK(pos + 23 <= size, "GX_LOAD_AURORA_TLUT read overrun");
    const auto idx = data[pos];
    pos += 1;
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    slot.data = reinterpret_cast<const void*>(read_u64(data + pos, bigEndian));
    pos += 8;
    slot.format = static_cast<GXTlutFmt>(read_u32(data + pos, bigEndian));
    pos += 4;
    slot.numEntries = read_u16(data + pos, bigEndian);
    pos += 2;
    slot.tlutObjId = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.tlutDataVersion = read_u32(data + pos, bigEndian);
    pos += 4;
    slot.set_no_cache(false); // Reset no-cache flag
    g_gxState.stateDirty = true;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_TEXOBJ) {
    CHECK(pos + 4 <= size, "GX_LOAD_AURORA_DESTROY_TEXOBJ read overrun");
    evict_texture_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_TLUT) {
    CHECK(pos + 4 <= size, "GX_LOAD_AURORA_DESTROY_TLUT read overrun");
    evict_tlut_object(read_u32(data + pos, bigEndian));
    pos += 4;
  } else if (subCmd == GX_LOAD_AURORA_DESTROY_COPY_TEX) {
    CHECK(pos + 8 <= size, "GX_LOAD_AURORA_DESTROY_COPY_TEX read overrun");
    evict_copy_texture(reinterpret_cast<const void*>(read_u64(data + pos, bigEndian)));
    pos += 8;
  } else if (subCmd == GX_LOAD_AURORA_INVALIDATE_TEX_ALL) {
    invalidate_static_texture_cache();
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_GROUP_PUSH) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_GROUP_POP) {
    aurora_pop_debug_group();
  } else if (subCmd == GX_LOAD_AURORA_DEBUG_MARKER_INSERT) {
    auto label = read_string(data, pos, size, bigEndian);
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    static u32 unknownAuroraLogCount = 0;
    if (unknownAuroraLogCount < 16) {
      Log.warn("Unknown Aurora subcommand: {:04X}; stopping FIFO decode", subCmd);
      ++unknownAuroraLogCount;
    }
    return false;
  }
  return true;
}

} // namespace aurora::gx::fifo
