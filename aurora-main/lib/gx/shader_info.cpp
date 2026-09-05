#include "shader_info.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#if !defined(_LIBCPP_VERSION)
#include <execution>
#endif
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <tracy/Tracy.hpp>

namespace aurora::gx {
// TODO: remove, just for testing
bool enableLodBias = true;

namespace {
Module Log("aurora::gx");

bool is_alpha_bump_channel(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

bool sampled_color_channel(GXChannelID id, size_t& out) {
  switch (id) {
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    out = 0;
    return true;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    out = 1;
    return true;
  default:
    return false;
  }
}

Fog fog_uniform() {
  const auto& state = g_gxState.fog;
  Fog fog{
      .color = state.color,
      .a = state.aRaw,
      .b = static_cast<float>(state.bMagnitude),
      .c = state.c,
      .pad = static_cast<float>(state.bShift),
  };

  const u32 rangeBase = g_gxState.fogRange[0];
  const bool rangeEnabled = (rangeBase & (1u << 10)) != 0;
  float rangeWidth = std::abs(g_gxState.xfViewport[0]) * 2.0f;
  if (rangeWidth <= 0.0f) {
    rangeWidth = 1.0f;
  }
  const int rangeCenter = static_cast<int>(rangeBase & 0x3ffu) - 342;
  const float screenSpaceCenter = rangeEnabled ? ((static_cast<float>(rangeCenter) / rangeWidth) * 2.0f) - 1.0f
                                               : 0.0f;
  fog.rangeBase = {screenSpaceCenter, rangeEnabled ? rangeWidth : 1.0f, 0.0f, 0.0f};

  std::array<float, 12> rangeK{};
  size_t kIndex = 0;
  for (size_t i = 1; i < g_gxState.fogRange.size(); ++i) {
    const u32 packed = g_gxState.fogRange[i];
    rangeK[kIndex++] = static_cast<float>((packed >> 12) & 0xfffu) / 64.0f;
    rangeK[kIndex++] = static_cast<float>(packed & 0xfffu) / 64.0f;
  }
  rangeK[10] = rangeK[9];
  rangeK[11] = rangeK[9];
  for (size_t i = 0; i < fog.rangeK.size(); ++i) {
    fog.rangeK[i] = {rangeK[i * 4 + 0], rangeK[i * 4 + 1], rangeK[i * 4 + 2], rangeK[i * 4 + 3]};
  }
  return fog;
}

float sanitize_light_dir_component(float value) noexcept {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  if ((bits & 0x7f800000u) != 0x7f800000u) {
    return value;
  }
  if ((bits & 0x007fffffu) != 0) {
    return 0.0f;
  }
  return (bits & 0x80000000u) != 0 ? -1.0f : 1.0f;
}
} // namespace

Light prepare_shader_light(Light light) noexcept {
  if (std::fabs(light.distAtt[0]) < 0.00001f && std::fabs(light.distAtt[1]) < 0.00001f &&
      std::fabs(light.distAtt[2]) < 0.00001f) {
    light.distAtt[0] = 0.00001f;
  }

  const double normSq = static_cast<double>(light.dir[0]) * static_cast<double>(light.dir[0]) +
                        static_cast<double>(light.dir[1]) * static_cast<double>(light.dir[1]) +
                        static_cast<double>(light.dir[2]) * static_cast<double>(light.dir[2]);
  if (normSq == 0.0) {
    light.dir[0] = 0.0f;
    light.dir[1] = 0.0f;
    light.dir[2] = 0.0f;
    return light;
  }
  const double invNorm = 1.0 / std::sqrt(normSq);
  // Aurora is built with fast-math, under which std::isnan/std::isinf may be optimized away.
  light.dir[0] = sanitize_light_dir_component(static_cast<float>(light.dir[0] * invNorm));
  light.dir[1] = sanitize_light_dir_component(static_cast<float>(light.dir[1] * invNorm));
  light.dir[2] = sanitize_light_dir_component(static_cast<float>(light.dir[2] * invNorm));
  return light;
}

namespace {
Vec4<float> texture_size_bias(const gfx::TextureBind& tex) {
  auto width = static_cast<float>(tex.texObj.width());
  auto height = static_cast<float>(tex.texObj.height());
  const auto vpBias =
      enableLodBias && tex.ref && tex.ref->hasArbitraryMips
          ? log2(std::min(g_gxState.renderViewport.width / std::max(g_gxState.logicalViewport.width, 1.f),
                          g_gxState.renderViewport.height / std::max(g_gxState.logicalViewport.height, 1.f)))
          : 0.f;
  return {width, height, tex.texObj.lod_bias() + vpBias, 0.0f};
}

Vec4<float> texcoord_scale(const TexCoordScale& scale) {
  return {static_cast<float>(scale.scaleS) + 1.0f, static_cast<float>(scale.scaleT) + 1.0f,
          scale.biasS ? 1.0f : 0.0f, scale.biasT ? 1.0f : 0.0f};
}

void mark_texture_sample(const TevStageTextureDependency& dependency, ShaderInfo& info) {
  if (!dependency.canSampleTexture) {
    return;
  }
  info.sampledTexCoords.set(static_cast<size_t>(dependency.texCoordId));
  info.sampledTextures.set(static_cast<size_t>(dependency.texMapId));
}

void mark_tev_reg_read(GXTevRegID reg, bool alpha, ShaderInfo& info) {
  auto& writes = alpha ? info.writesTevRegAlpha : info.writesTevRegRgb;
  auto& loads = alpha ? info.loadsTevRegAlpha : info.loadsTevRegRgb;
  if (!writes.test(reg)) {
    loads.set(reg);
  }
}

void color_arg_reg_info(GXTevColorArg arg, const TevStage& stage,
                        const TevStageTextureDependency& textureDependency, ShaderInfo& info) {
  switch (arg) {
  case GX_CC_CPREV:
    mark_tev_reg_read(GX_TEVPREV, false, info);
    break;
  case GX_CC_APREV:
    mark_tev_reg_read(GX_TEVPREV, true, info);
    break;
  case GX_CC_C0:
    mark_tev_reg_read(GX_TEVREG0, false, info);
    break;
  case GX_CC_A0:
    mark_tev_reg_read(GX_TEVREG0, true, info);
    break;
  case GX_CC_C1:
    mark_tev_reg_read(GX_TEVREG1, false, info);
    break;
  case GX_CC_A1:
    mark_tev_reg_read(GX_TEVREG1, true, info);
    break;
  case GX_CC_C2:
    mark_tev_reg_read(GX_TEVREG2, false, info);
    break;
  case GX_CC_A2:
    mark_tev_reg_read(GX_TEVREG2, true, info);
    break;
  case GX_CC_TEXC:
  case GX_CC_TEXA:
    mark_texture_sample(textureDependency, info);
    break;
  case GX_CC_RASC:
  case GX_CC_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      size_t channel = 0;
      if (sampled_color_channel(stage.channelId, channel)) {
        info.sampledColorChannels.set(channel);
      }
    }
    break;
  case GX_CC_KONST:
    switch (stage.kcSel) {
    case GX_TEV_KCSEL_K0:
    case GX_TEV_KCSEL_K0_R:
    case GX_TEV_KCSEL_K0_G:
    case GX_TEV_KCSEL_K0_B:
    case GX_TEV_KCSEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KCSEL_K1:
    case GX_TEV_KCSEL_K1_R:
    case GX_TEV_KCSEL_K1_G:
    case GX_TEV_KCSEL_K1_B:
    case GX_TEV_KCSEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KCSEL_K2:
    case GX_TEV_KCSEL_K2_R:
    case GX_TEV_KCSEL_K2_G:
    case GX_TEV_KCSEL_K2_B:
    case GX_TEV_KCSEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KCSEL_K3:
    case GX_TEV_KCSEL_K3_R:
    case GX_TEV_KCSEL_K3_G:
    case GX_TEV_KCSEL_K3_B:
    case GX_TEV_KCSEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}

void alpha_arg_reg_info(GXTevAlphaArg arg, const TevStage& stage,
                        const TevStageTextureDependency& textureDependency, ShaderInfo& info) {
  switch (arg) {
  case GX_CA_APREV:
    mark_tev_reg_read(GX_TEVPREV, true, info);
    break;
  case GX_CA_A0:
    mark_tev_reg_read(GX_TEVREG0, true, info);
    break;
  case GX_CA_A1:
    mark_tev_reg_read(GX_TEVREG1, true, info);
    break;
  case GX_CA_A2:
    mark_tev_reg_read(GX_TEVREG2, true, info);
    break;
  case GX_CA_TEXA:
    mark_texture_sample(textureDependency, info);
    break;
  case GX_CA_RASA:
    if (stage.channelId != GX_COLOR_NULL && stage.channelId != GX_COLOR_ZERO &&
        !is_alpha_bump_channel(stage.channelId)) {
      size_t channel = 0;
      if (sampled_color_channel(stage.channelId, channel)) {
        info.sampledColorChannels.set(channel);
      }
    }
    break;
  case GX_CA_KONST:
    switch (stage.kaSel) {
    case GX_TEV_KASEL_K0_R:
    case GX_TEV_KASEL_K0_G:
    case GX_TEV_KASEL_K0_B:
    case GX_TEV_KASEL_K0_A:
      info.sampledKColors.set(0);
      break;
    case GX_TEV_KASEL_K1_R:
    case GX_TEV_KASEL_K1_G:
    case GX_TEV_KASEL_K1_B:
    case GX_TEV_KASEL_K1_A:
      info.sampledKColors.set(1);
      break;
    case GX_TEV_KASEL_K2_R:
    case GX_TEV_KASEL_K2_G:
    case GX_TEV_KASEL_K2_B:
    case GX_TEV_KASEL_K2_A:
      info.sampledKColors.set(2);
      break;
    case GX_TEV_KASEL_K3_R:
    case GX_TEV_KASEL_K3_G:
    case GX_TEV_KASEL_K3_B:
    case GX_TEV_KASEL_K3_A:
      info.sampledKColors.set(3);
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
}
} // namespace

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  ZoneScoped;

  ShaderInfo info{
      // vtx_start, current_pnmtx, render/logical viewport size, array_start, pad, proj
      .uniformSize = 4 + 4 + 8 + 8 + 8 + 48 + 64,
  };

  if (config.lineMode != 0) {
    info.uniformSize += 4 + 4 + 4 + 4; // line_width, line_aspect_y, line_tex_offset, line_texcoord_mask
    info.lineMode = config.lineMode;
  }

  for (int attr = 0; attr < config.attrs.size(); attr++) {
    const auto attrType = config.attrs[attr].attrType;
    if ((attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX)) && attrType == GX_DIRECT) {
      info.indexAttr.set(attr);
    } else if (attrType == GX_INDEX8 || attrType == GX_INDEX16) {
      info.indexAttr.set(attr);
    }
  }

  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));
    // Color pass
    color_arg_reg_info(stage.colorPass.a, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.b, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.c, stage, textureDependency, info);
    color_arg_reg_info(stage.colorPass.d, stage, textureDependency, info);
    info.writesTevRegRgb.set(stage.colorOp.outReg);

    // Alpha pass
    alpha_arg_reg_info(stage.alphaPass.a, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.b, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.c, stage, textureDependency, info);
    alpha_arg_reg_info(stage.alphaPass.d, stage, textureDependency, info);
    info.writesTevRegAlpha.set(stage.alphaOp.outReg);
  }
  if (const int zTexStage = tev_z_texture_stage(config); zTexStage >= 0) {
    mark_texture_sample(tev_stage_texture_dependency(config, static_cast<u32>(zTexStage)), info);
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));

    // Coordinate state is independent of whether this stage has an enabled texture map.
    if (textureDependency.needsFixedTexcoordState && textureDependency.texCoordId >= 0) {
      info.sampledTexCoords.set(static_cast<size_t>(textureDependency.texCoordId));
    }

    // Skip if not enabled
    if (stage.indTexStage >= config.numIndStages) {
      continue;
    }
    const bool usesIndStage = stage.indTexMtxId != GX_ITM_OFF || is_alpha_bump_channel(stage.channelId);
    if (!usesIndStage) {
      continue;
    }
    info.usedIndStages.set(stage.indTexStage);
    const auto& indStage = config.indStages[stage.indTexStage];
    if (indStage.texMapId < MaxTextures) {
      info.sampledTextures.set(indStage.texMapId);
      info.sampledIndTextures.set(indStage.texMapId);
    }
    const int effectiveIndTexCoord = tev_effective_texcoord(config, indStage.texCoordId);
    if (effectiveIndTexCoord >= 0) {
      info.sampledTexCoords.set(static_cast<size_t>(effectiveIndTexCoord));
    }
    // Track which indirect matrix is used
    if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_0);
    } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_S0);
    } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2) {
      info.usedIndTexMtxs.set(stage.indTexMtxId - GX_ITM_T0);
    }
  }

  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  info.uniformSize += loadsTevRegs.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cc.lightingEnabled || cca.lightingEnabled) {
        info.lightingEnabled = true;
      }
    }
  }
  if (info.lightingEnabled) {
    // Lights + light state for all channels
    info.uniformSize += 16 + sizeof(Light) * GX::MaxLights;
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (info.sampledColorChannels.test(i)) {
      const auto& cc = config.colorChannels[i];
      if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cc.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      const auto& cca = config.colorChannels[i + GX_ALPHA0];
      if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
      if (cca.matSrc == GX_SRC_REG) {
        info.uniformSize += sizeof(Vec4<float>);
      }
    }
  }
  info.uniformSize += info.sampledKColors.count() * sizeof(Vec4<float>);
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (config.dualTexEnabled && tcg.postMtx != GX_PTIDENTITY) {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      info.usesPTTexMtx.set(postMtxIdx);
    }
  }
  if (info.usesPTTexMtx.any())
    info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;

  // Resolve the matrix slots this shader can read.
  {
    auto& layout = info.matrixLayout;
    layout.postexRemap.fill(UniformMatrixLayout::kAbsent);

    const bool dynamicPnMtx = config.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE;
    bool dynamicTexMtx = false;
    std::bitset<MaxPostexMtx> literalSlots;
    for (size_t i = 0; i < info.sampledTexCoords.size(); ++i) {
      if (!info.sampledTexCoords.test(i)) {
        continue;
      }
      const auto& tcg = config.tcgs[i];
      if (tcg.type != GX_TG_MTX2x4 && tcg.type != GX_TG_MTX3x4) {
        continue;
      }
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        // A per-vertex texture matrix index addresses raw matrix memory and can name any row, including the position rows.
        dynamicTexMtx = true;
        continue;
      }
      if (tcg.mtx == GX_IDENTITY) {
        continue;
      }
      const u32 slot = static_cast<u32>(tcg.mtx) / 3;
      if (slot >= MaxPostexMtx) {
        // Out of range for the shader array; preserve the uncompacted layout rather than inventing a different (still invalid) index.
        dynamicTexMtx = true;
        continue;
      }
      literalSlots.set(slot);
    }

    // At most MaxPostexMtx entries can ever be pushed: the absolute layout pushes the ten position slots plus at most ten texture slots, and the compacted layout pushes the current matrix plus at most one literal slot per texgen.
    const auto pushPostex = [&](u8 slot) {
      CHECK(layout.postexCount < layout.postexSlots.size(), "postex matrix layout overflow");
      layout.postexSlots[layout.postexCount++] = slot;
    };
    const auto pushNrm = [&](u8 slot) {
      CHECK(layout.nrmCount < layout.nrmSlots.size(), "normal matrix layout overflow");
      layout.nrmSlots[layout.nrmCount++] = slot;
    };

    layout.absolutePosRegion = dynamicPnMtx || dynamicTexMtx;
    if (!layout.absolutePosRegion) {
      // Compact slot 0 always holds the matrix selected by the current matrix index; build_uniform writes 0 into `current_pnmtx` to match.
      pushPostex(UniformMatrixLayout::kCurrentPnMtx);
      pushNrm(UniformMatrixLayout::kCurrentPnMtx);
    }
    for (u32 slot = 0; slot < MaxPnMtx; ++slot) {
      if (!layout.absolutePosRegion && !literalSlots.test(slot)) {
        continue;
      }
      layout.postexRemap[slot] = layout.postexCount;
      pushPostex(static_cast<u8>(slot));
      if (layout.absolutePosRegion) {
        // The normal array is indexed by the same expression as the position array, so its compact indices have to line up with the position slots.
        pushNrm(static_cast<u8>(slot));
      }
    }
    for (u32 slot = MaxPnMtx; slot < MaxPostexMtx; ++slot) {
      if (!dynamicTexMtx && !literalSlots.test(slot)) {
        continue;
      }
      layout.postexRemap[slot] = layout.postexCount;
      pushPostex(static_cast<u8>(slot));
    }
    // `postex_mtx[in_pnmtxidx]` and `nrm_mtx[in_pnmtxidx]` are emitted unconditionally, so neither array can ever be empty.
    info.uniformSize += sizeof(Mat3x4<float>) * (layout.postexCount + layout.nrmCount);
  }
  if (config.fogType != GX_FOG_NONE) {
    info.usesFog = true;
    info.uniformSize += sizeof(Fog);
  }
  if (info.usedIndTexMtxs.any()) {
    info.uniformSize += MaxIndTexMtxs * 2 * sizeof(Vec4<s32>);
  }
  info.uniformSize += info.sampledTexCoords.count() * sizeof(Vec4<float>);
  info.uniformSize += info.sampledTextures.count() * sizeof(Vec4<float>);
  info.uniformSize = gfx::align_uniform(info.uniformSize);
  if (info.uniformSize > MaxUniformSize) {
    Log.fatal("Uniform size exceeds maximum: {} > {}", info.uniformSize, MaxUniformSize);
  }
  return info;
}

static f32 tex_offset(GXTexOffset offs) noexcept {
  switch (offs) {
    DEFAULT_FATAL("invalid tex offset {}", underlying(offs));
  case GX_TO_ZERO:
    return 0.f;
  case GX_TO_SIXTEENTH:
    return 1.f / 16.f;
  case GX_TO_EIGHTH:
    return 1.f / 8.f;
  case GX_TO_FOURTH:
    return 1.f / 4.f;
  case GX_TO_HALF:
    return 1.f / 2.f;
  case GX_TO_ONE:
    return 1.f;
  }
}

static u32 point_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].pointOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

static u32 line_texcoord_mask() noexcept {
  u32 mask = 0;
  for (int i = 0; i < MaxTexCoord; ++i) {
    if (g_gxState.texCoordScales[i].lineOffset) {
      mask |= 1 << i;
    }
  }
  return mask;
}

namespace {
// Largest possible staged prefix: scalar head (80) + line/point block (16) + projection matrix + every matrix the uncompacted layout can hold.
constexpr size_t kStagedUniformBytes =
    96 + sizeof(Mat4x4<float>) + sizeof(Mat3x4<float>) * (MaxPostexMtx + MaxPnMtx);

// The host viewport always receives the normalized GX depth window (render_pass_impl clamps to minDepth <= maxDepth).
//
// Folds the near/far depth correction the vertex shader used to apply per-vertex directly into the
// projection matrix instead (matching upstream aurora commit 1dde08fa, "Move depth correction to
// projection matrix") - valid because the correction is a linear combination of the z/w rows, so
// applying it once here to the row is equivalent to applying it once per-vertex to the dot product,
// and it must be applied exactly once: doing it here AND in the shader (the previous bug) canceled
// the negation out for `flip`, silently making "reversed" Z behave identically to forward Z.
// `flip` decides which of the two single-application forms this draw needs: true bakes in the
// reversed-Z inversion (z' = -z), false bakes in the forward-Z near/far combination (z' = z + w) -
// exactly one always applies, never both, and never neither.
static Mat4x4<float> effective_projection() noexcept {
  const auto& vp = g_gxState.renderViewport;
  const bool flip = (vp.znear <= vp.zfar) == UseReversedZ;
  Mat4x4<float> proj = g_gxState.proj;
  for (size_t i = 0; i < 4; ++i) {
    proj.m2.m[i] = flip ? -proj.m2.m[i] : (proj.m2.m[i] + proj.m3.m[i]);
  }
  return proj;
}
} // namespace

UniformRanges build_uniform(const ShaderInfo& info, u32 vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask) noexcept {
  ZoneScoped;

  auto [buf, range] = gfx::map_uniform(info.uniformSize);

  const auto& layout = info.matrixLayout;
  // `postex_mtx[current_pnmtx]` indexes raw matrix memory, so a current-matrix index of 10 or more selects a texture matrix; the normal array only covers the position rows and clamps.
  const u32 currentPostexSlot = std::min<u32>(g_gxState.currentPnMtx, MaxPostexMtx - 1);
  const u32 currentNrmSlot = std::min<u32>(g_gxState.currentPnMtx, MaxPnMtx - 1);

  // The mapped uniform range lives in write-combine memory, which wants long sequential stores.
  alignas(16) std::array<uint8_t, kStagedUniformBytes> staged;
  size_t stagedSize = 0;
  const auto stage = [&](const void* data, size_t size) noexcept {
    std::memcpy(staged.data() + stagedSize, data, size);
    stagedSize += size;
  };
  const auto stage_u32 = [&](u32 value) noexcept { stage(&value, sizeof(value)); };
  const auto stage_f32 = [&](f32 value) noexcept { stage(&value, sizeof(value)); };

  stage_u32(vtxStart);
  // With a compacted position region the live matrix is uploaded to slot 0, so every `postex_mtx[in_pnmtxidx]` / `nrm_mtx[in_pnmtxidx]` read has to resolve to 0 as well.
  stage_u32(layout.absolutePosRegion ? g_gxState.currentPnMtx : 0u);
  stage_f32(g_gxState.renderViewport.width);
  stage_f32(g_gxState.renderViewport.height);
  stage_f32(g_gxState.logicalViewport.width);
  stage_f32(g_gxState.logicalViewport.height);
  std::memset(staged.data() + stagedSize, 0, 8); // pad
  stagedSize += 8;
  for (const auto& vaRange : ranges.vaRanges) {
    stage_u32(vaRange.offset);
  }
  if (info.lineMode != 0) {
    if (info.lineMode == 3) { // GX_POINTS
      stage_f32(static_cast<f32>(g_gxState.pointSize) / 6.f);
      stage_f32(1.0f);
      stage_f32(tex_offset(g_gxState.pointTexOffset));
      stage_u32(point_texcoord_mask());
    } else { // GX_LINES / GX_LINESTRIP
      stage_f32(static_cast<f32>(g_gxState.lineWidth) / 6.f);
      stage_f32(g_gxState.lineHalfAspect ? 0.5f : 1.f);
      stage_f32(tex_offset(g_gxState.lineTexOffset));
      stage_u32(line_texcoord_mask());
    }
  }
  const size_t projectionOffset = stagedSize;
  const Mat4x4<float> effectiveProj = effective_projection();
  stage(&effectiveProj, sizeof(effectiveProj));

  const size_t positionOffset = stagedSize;
  for (u32 i = 0; i < layout.postexCount; ++i) {
    const u32 slot =
        layout.postexSlots[i] == UniformMatrixLayout::kCurrentPnMtx ? currentPostexSlot
                                                                    : layout.postexSlots[i];
    stage(slot < MaxPnMtx ? &g_gxState.pnMtx[slot].pos : &g_gxState.texMtxs[slot - MaxPnMtx],
          sizeof(Mat3x4<float>));
  }

  const size_t normalOffset = stagedSize;
  for (u32 i = 0; i < layout.nrmCount; ++i) {
    const u32 slot = layout.nrmSlots[i] == UniformMatrixLayout::kCurrentPnMtx ? currentNrmSlot
                                                                             : layout.nrmSlots[i];
    stage(&g_gxState.pnMtx[slot].nrm, sizeof(Mat3x4<float>));
  }
  buf.append(staged.data(), stagedSize);

  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  for (int i = 0; i < loadsTevRegs.size(); ++i) {
    if (loadsTevRegs.test(i)) {
      buf.append(g_gxState.colorRegs[i]);
    }
  }
  if (info.lightingEnabled) {
    // Sanitizing and normalizing light directions is substantially more expensive than copying the uniform data, while lights normally remain unchanged across many draws.
    static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
    if (g_gxState.preparedLightsDirty) {
      for (size_t i = 0; i < g_gxState.preparedLights.size(); ++i) {
        g_gxState.preparedLights[i] = prepare_shader_light(g_gxState.lights[i]);
      }
      g_gxState.preparedLightsDirty = false;
    }
    buf.append(g_gxState.preparedLights);
    // Light state for all channels
    for (int i = 0; i < 4; ++i) {
      buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
    }
  }
  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }
    const auto& ccc = g_gxState.colorChannelConfig[i];
    const auto& ccs = g_gxState.colorChannelState[i];
    if (ccc.lightingEnabled && ccc.ambSrc == GX_SRC_REG) {
      buf.append(ccs.ambColor);
    }
    if (ccc.matSrc == GX_SRC_REG) {
      buf.append(ccs.matColor);
    }
    const auto& ccca = g_gxState.colorChannelConfig[i + GX_ALPHA0];
    const auto& ccsa = g_gxState.colorChannelState[i + GX_ALPHA0];
    if (ccca.lightingEnabled && ccca.ambSrc == GX_SRC_REG) {
      buf.append(ccsa.ambColor);
    }
    if (ccca.matSrc == GX_SRC_REG) {
      buf.append(ccsa.matColor);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      buf.append(g_gxState.kcolors[i]);
    }
  }
  if (info.usesPTTexMtx.any()) {
    for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
      buf.append(g_gxState.ptTexMtxs[i]);
    }
  }
  if (info.usesFog) {
    buf.append(fog_uniform());
  }
  if (info.usedIndTexMtxs.any()) {
    for (int i = 0; i < MaxIndTexMtxs; ++i) {
      const auto& mtx = g_gxState.indTexMtxs[i];
      buf.append(Vec4<s32>{indirect_matrix_mantissa(mtx.mtx.m0.x), indirect_matrix_mantissa(mtx.mtx.m0.y),
                           indirect_matrix_mantissa(mtx.mtx.m1.x), indirect_matrix_mantissa(mtx.mtx.m1.y)});
      buf.append(Vec4<s32>{indirect_matrix_mantissa(mtx.mtx.m2.x), indirect_matrix_mantissa(mtx.mtx.m2.y),
                           indirect_matrix_shift(mtx.scaleExp), 0});
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (info.sampledTexCoords.test(i)) {
      buf.append(texcoord_scale(g_gxState.texCoordScales[i]));
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    const auto& tex = get_texture(static_cast<GXTexMapID>(i));
    // CHECK(tex, "unbound texture {}", i);
    buf.append(texture_size_bias(tex));
  }

  if (!perspective || frame_interpolation_fps() == 0) {
    g_gxState.stateDirty = false;
    return {
        .current = range,
        .interpolated = {},
    };
  }

  const auto interpolatedRanges = record_interpolation_draw(
      drawIdentity, effectiveProj, usedPnMtxMask,
      InterpolatedUniformLayout{
          .sourceUniformData = buf.data(),
          .uniformSize = range.size,
          .projectionOffset = projectionOffset,
          .positionOffset = positionOffset,
          .normalOffset = normalOffset,
          // A compacted position region holds the current matrix at slot 0.
          .currentMatrix = layout.absolutePosRegion
                               ? std::min<size_t>(g_gxState.currentPnMtx, MaxPnMtx - 1)
                               : 0,
          .indexedMatrices = info.indexAttr.test(GX_VA_PNMTXIDX),
      });
  g_gxState.stateDirty = false;
  return {
      .current = range,
      .interpolated = interpolatedRanges,
  };
}
} // namespace aurora::gx
