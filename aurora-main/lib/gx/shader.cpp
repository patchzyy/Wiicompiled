#include "../gfx/common.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>
#include <mutex>
#include <string_view>
#include <utility>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

static Module Log("aurora::gfx::gx");


static inline std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "?";
  }
}

static bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

static std::string_view disabled_texture_color(const ShaderConfig& config) noexcept {
  return config.numTexGens == 0 ? "vec3f(0.0)"sv : "vec3f(1.0)"sv;
}

static std::string_view disabled_texture_alpha(const ShaderConfig& config) noexcept {
  return config.numTexGens == 0 ? "0.0"sv : "1.0"sv;
}

static std::string tev_mask_expr(const std::string& value, u32 mask) {
  // t_IndTexCoord is already expanded into the 0..255 indirect sample domain.
  return fmt::format("(f32(u32({}) & 0x{:X}u) / 255.0)", value, mask);
}

static std::string alpha_bump_sel(size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  if (stage.indTexStage >= config.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }

  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
    DEFAULT_FATAL("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  }

  switch (stage.indTexFormat) {
    DEFAULT_FATAL("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  case GX_ITF_8:
    return tev_mask_expr(baseCoord, 0xF8u);
  case GX_ITF_5:
    return tev_mask_expr(baseCoord, 0xE0u);
  case GX_ITF_4:
    return tev_mask_expr(baseCoord, 0xF0u);
  case GX_ITF_3:
    return tev_mask_expr(baseCoord, 0xF8u);
  }
}

u8 color_channel(GXChannelID id) noexcept {
  switch (id) {
    DEFAULT_FATAL("unimplemented color channel {}", id);
  case GX_COLOR0:
  case GX_ALPHA0:
  case GX_COLOR0A0:
    return 0;
  case GX_COLOR1:
  case GX_ALPHA1:
  case GX_COLOR1A1:
    return 1;
  }
}

static std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid color arg {}", underlying(arg));
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3f(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3f(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3f(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3f(tevreg2.a)";
  case GX_CC_TEXC: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return std::string(disabled_texture_color(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return fmt::format("vec3f({})", disabled_texture_alpha(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3f(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("vec3f(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3f(1.0)";
  case GX_CC_HALF:
    return "vec3f(0.5)";
  case GX_CC_KONST: {
    switch (stage.kcSel) {
      DEFAULT_FATAL("invalid kcSel {}", underlying(stage.kcSel));
    case GX_TEV_KCSEL_8_8:
      return "vec3f(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3f(7.0/8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3f(6.0/8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3f(5.0/8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3f(4.0/8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3f(3.0/8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3f(2.0/8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3f(1.0/8.0)";
    case GX_TEV_KCSEL_K0:
      return "ubuf.kcolor0.rgb";
    case GX_TEV_KCSEL_K1:
      return "ubuf.kcolor1.rgb";
    case GX_TEV_KCSEL_K2:
      return "ubuf.kcolor2.rgb";
    case GX_TEV_KCSEL_K3:
      return "ubuf.kcolor3.rgb";
    case GX_TEV_KCSEL_K0_R:
      return "vec3f(ubuf.kcolor0.r)";
    case GX_TEV_KCSEL_K1_R:
      return "vec3f(ubuf.kcolor1.r)";
    case GX_TEV_KCSEL_K2_R:
      return "vec3f(ubuf.kcolor2.r)";
    case GX_TEV_KCSEL_K3_R:
      return "vec3f(ubuf.kcolor3.r)";
    case GX_TEV_KCSEL_K0_G:
      return "vec3f(ubuf.kcolor0.g)";
    case GX_TEV_KCSEL_K1_G:
      return "vec3f(ubuf.kcolor1.g)";
    case GX_TEV_KCSEL_K2_G:
      return "vec3f(ubuf.kcolor2.g)";
    case GX_TEV_KCSEL_K3_G:
      return "vec3f(ubuf.kcolor3.g)";
    case GX_TEV_KCSEL_K0_B:
      return "vec3f(ubuf.kcolor0.b)";
    case GX_TEV_KCSEL_K1_B:
      return "vec3f(ubuf.kcolor1.b)";
    case GX_TEV_KCSEL_K2_B:
      return "vec3f(ubuf.kcolor2.b)";
    case GX_TEV_KCSEL_K3_B:
      return "vec3f(ubuf.kcolor3.b)";
    case GX_TEV_KCSEL_K0_A:
      return "vec3f(ubuf.kcolor0.a)";
    case GX_TEV_KCSEL_K1_A:
      return "vec3f(ubuf.kcolor1.a)";
    case GX_TEV_KCSEL_K2_A:
      return "vec3f(ubuf.kcolor2.a)";
    case GX_TEV_KCSEL_K3_A:
      return "vec3f(ubuf.kcolor3.a)";
    }
  }
  case GX_CC_ZERO:
    return "vec3f(0.0)";
  }
}

static std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config,
                                 const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid alpha arg {}", underlying(arg));
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (!tev_stage_texture_dependency(config, static_cast<u32>(stageIdx)).canSampleTexture) {
      return std::string(disabled_texture_alpha(config));
    }
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    switch (stage.kaSel) {
      DEFAULT_FATAL("invalid kaSel {}", underlying(stage.kaSel));
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0/8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0/8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0/8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0/8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0/8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0/8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0/8.0)";
    case GX_TEV_KASEL_K0_R:
      return "ubuf.kcolor0.r";
    case GX_TEV_KASEL_K1_R:
      return "ubuf.kcolor1.r";
    case GX_TEV_KASEL_K2_R:
      return "ubuf.kcolor2.r";
    case GX_TEV_KASEL_K3_R:
      return "ubuf.kcolor3.r";
    case GX_TEV_KASEL_K0_G:
      return "ubuf.kcolor0.g";
    case GX_TEV_KASEL_K1_G:
      return "ubuf.kcolor1.g";
    case GX_TEV_KASEL_K2_G:
      return "ubuf.kcolor2.g";
    case GX_TEV_KASEL_K3_G:
      return "ubuf.kcolor3.g";
    case GX_TEV_KASEL_K0_B:
      return "ubuf.kcolor0.b";
    case GX_TEV_KASEL_K1_B:
      return "ubuf.kcolor1.b";
    case GX_TEV_KASEL_K2_B:
      return "ubuf.kcolor2.b";
    case GX_TEV_KASEL_K3_B:
      return "ubuf.kcolor3.b";
    case GX_TEV_KASEL_K0_A:
      return "ubuf.kcolor0.a";
    case GX_TEV_KASEL_K1_A:
      return "ubuf.kcolor1.a";
    case GX_TEV_KASEL_K2_A:
      return "ubuf.kcolor2.a";
    case GX_TEV_KASEL_K3_A:
      return "ubuf.kcolor3.a";
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  }
}

static std::string tev_bias_i32(GXTevBias bias) {
  switch (bias) {
    DEFAULT_FATAL("invalid tev bias {}", underlying(bias));
  case GX_TB_ZERO:
    return "0";
  case GX_TB_ADDHALF:
    return "128";
  case GX_TB_SUBHALF:
    return "-128";
  }
}

static std::string tev_scale_index(GXTevScale scale) {
  switch (scale) {
    DEFAULT_FATAL("invalid tev scale {}", underlying(scale));
  case GX_CS_SCALE_1:
    return "0u";
  case GX_CS_SCALE_2:
    return "1u";
  case GX_CS_SCALE_4:
    return "2u";
  case GX_CS_DIVIDE_2:
    return "3u";
  }
}

static std::string tev_regular_op(GXTevOp op, GXTevBias bias, GXTevScale scale, std::string_view a,
                                  std::string_view b, std::string_view c, std::string_view d,
                                  std::string_view suffix) {
  CHECK(op == GX_TEV_ADD || op == GX_TEV_SUB, "invalid regular tev op {}", underlying(op));
  return fmt::format("tev_regular_{4}({0}, {1}, {2}, {3}, {5}, {6}, {7})", a, b, c, d, suffix,
                     tev_bias_i32(bias), tev_scale_index(scale), op == GX_TEV_SUB ? "true" : "false");
}

static std::string tev_op(GXTevOp op, GXTevBias bias, GXTevScale scale, std::string_view a, std::string_view b,
                          std::string_view c, std::string_view d, std::string_view zero,
                          std::string_view suffix) {
  switch (op) {
    DEFAULT_FATAL("unimplemented tev op {}", underlying(op));
  case GX_TEV_ADD:
  case GX_TEV_SUB:
    return tev_regular_op(op, bias, scale, a, b, c, d, suffix);
  case GX_TEV_COMP_R8_GT:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) > round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) == round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_GR16_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) > round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) == round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_GT:
    return fmt::format("select({3}, {2}, round({0} * 255.0) > round({1} * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_EQ:
    return fmt::format("select({3}, {2}, round({0} * 255.0) == round({1} * 255.0)) + {4}", a, b, c, zero, d);
  }
}

static std::string tev_color_op(GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d) {
  const auto overflow = [](std::string_view reg) { return fmt::format("tev_overflow_vec3f({})", reg); };
  std::string expr = tev_op(op, bias, scale, overflow(a), overflow(b), overflow(c), d, "vec3(0)"sv, "vec3f"sv);
  return clamp ? fmt::format("clamp({}, vec3f(0.0), vec3f(1.0))", expr)
               : fmt::format("clamp({}, vec3f(-1024.0 / 255.0), vec3f(1023.0 / 255.0))", expr);
}

static bool alpha_compare_uses_color_inputs(GXTevOp op) noexcept {
  switch (op) {
  case GX_TEV_COMP_R8_GT:
  case GX_TEV_COMP_R8_EQ:
  case GX_TEV_COMP_GR16_GT:
  case GX_TEV_COMP_GR16_EQ:
  case GX_TEV_COMP_BGR24_GT:
  case GX_TEV_COMP_BGR24_EQ:
    return true;
  default:
    return false;
  }
}

static std::string tev_alpha_op(GXTevOp op, GXTevBias bias, GXTevScale scale, bool clamp,
                                std::string_view a, std::string_view b, std::string_view c, std::string_view d,
                                std::string_view colorA, std::string_view colorB) {
  const auto scalarOverflow = [](std::string_view reg) { return fmt::format("tev_overflow_f32({})", reg); };
  std::string expr;
  if (alpha_compare_uses_color_inputs(op)) {
    const auto colorOverflow = [](std::string_view reg) { return fmt::format("tev_overflow_vec3f({})", reg); };
    // GX alpha compares R8/GR16/BGR24 using the color combiner's A/B inputs, while C and D remain the scalar alpha-combiner inputs.
    expr = tev_op(op, bias, scale, colorOverflow(colorA), colorOverflow(colorB), scalarOverflow(c), d, "0.0"sv,
                  "f32"sv);
  } else {
    // The numeric RGB8 op aliases are the alpha combiner's A8 compares and therefore continue to compare scalar alpha A/B inputs here.
    expr = tev_op(op, bias, scale, scalarOverflow(a), scalarOverflow(b), scalarOverflow(c), d, "0.0"sv, "f32"sv);
  }
  return clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -1024.0 / 255.0, 1023.0 / 255.0)", expr);
}

struct AlphaCompareExpr {
  std::string expr;
  int constant = -1;
};

static AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

static AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& expr) {
  if (expr.constant != -1) {
    return alpha_compare_const(expr.constant == 0);
  }
  return {fmt::format("!{}", expr.expr), -1};
}

static AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 0 || rhs.constant == 0) {
    return alpha_compare_const(false);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} && {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 1 || rhs.constant == 1) {
    return alpha_compare_const(true);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  return {fmt::format("({} || {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant != rhs.constant);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  if (lhs.constant == 1) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 1) {
    return alpha_compare_not(lhs);
  }
  return {fmt::format("({} != {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant == rhs.constant);
  }
  if (lhs.constant == 0) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 0) {
    return alpha_compare_not(lhs);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} == {})", lhs.expr, rhs.expr), -1};
}

static AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
  default:
    return alpha_compare_const(true);
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}

static inline std::string vtx_attr(const ShaderConfig& config, GXAttr attr) {
  const auto type = config.attrs[attr].attrType;
  if (type == GX_NONE) {
    if (attr == GX_VA_PNMTXIDX) {
      return "ubuf.current_pnmtx";
    }
    if (attr == GX_VA_NRM) {
      // Default normal
      return "vec3f(1.0, 0.0, 0.0)"s;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(1.0)"s;
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));
  }
  if (attr == GX_VA_POS) {
    return "in_pos"s;
  }
  if (attr == GX_VA_NRM) {
    return "in_nrm"s;
  }
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    const auto idx = attr - GX_VA_CLR0;
    return fmt::format("in_clr{}", idx);
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto idx = attr - GX_VA_TEX0;
    return fmt::format("in_tex{}_uv", idx);
  }
  if (attr == GX_VA_PNMTXIDX) {
    return "in_pnmtxidx"s;
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto idx = attr - GX_VA_TEX0MTXIDX;
    return fmt::format("in_texmtxidx{}", idx);
  }
  UNLIKELY FATAL("unhandled vtx attr {}", underlying(attr));
}

static std::string vertex_color_attr(const ShaderConfig& config, u32 channel) {
  const int attr = shader_vertex_color_attr(config, channel);
  return attr >= 0 ? vtx_attr(config, static_cast<GXAttr>(attr)) : "vec4f(1.0)"s;
}

constexpr std::array<std::string_view, GX_CC_ZERO + 1> TevColorArgNames{
    "CPREV"sv, "APREV"sv, "C0"sv,   "A0"sv,   "C1"sv,  "A1"sv,   "C2"sv,    "A2"sv,
    "TEXC"sv,  "TEXA"sv,  "RASC"sv, "RASA"sv, "ONE"sv, "HALF"sv, "KONST"sv, "ZERO"sv,
};
constexpr std::array<std::string_view, GX_CA_ZERO + 1> TevAlphaArgNames{
    "APREV"sv, "A0"sv, "A1"sv, "A2"sv, "TEXA"sv, "RASA"sv, "KONST"sv, "ZERO"sv,
};

auto fetch_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le, u8 cntOverride = 0) -> std::string {
  const u8 cnt = cntOverride != 0 ? cntOverride : mapping.cnt;
  switch (mapping.compType) {
  case GX_U8:
    return fmt::format("fetch_u8_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_S8:
    return fmt::format("fetch_s8_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_U16:
    return fmt::format("fetch_u16_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_S16:
    return fmt::format("fetch_s16_{}(&{}, {}, {}, {})", cnt, buf, offs, mapping.frac, le);
  case GX_F32:
    return fmt::format("fetch_f32_{}(&{}, {}, {})", cnt, buf, offs, le);
  case GX_RGBA8:
    return fmt::format("unpack4x8unorm(load_u32_raw(&{}, {}))", buf, offs);
  default:
    Log.fatal("fetch_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto fetch_color_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_RGB565:
    return fmt::format("fetch_rgb565(&{}, {}, {})", buf, offs, le);
  case GX_RGB8:
    return fmt::format("fetch_rgb8(&{}, {}, {})", buf, offs, le);
  case GX_RGBX8:
    return fmt::format("fetch_rgbx8(&{}, {}, {})", buf, offs, le);
  case GX_RGBA4:
    return fmt::format("fetch_rgba4(&{}, {}, {})", buf, offs, le);
  case GX_RGBA6:
    return fmt::format("fetch_rgba6(&{}, {}, {})", buf, offs, le);
  case GX_RGBA8:
    return fmt::format("fetch_rgba8(&{}, {}, {})", buf, offs, le);
  default:
    Log.fatal("fetch_color_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string;

static u32 attr_comp_type_size(u8 compType) noexcept {
  switch (static_cast<GXCompType>(compType)) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  case GX_F32:
    return 4;
  default:
    Log.fatal("attr_comp_type_size: Unimplemented {}", static_cast<GXCompType>(compType));
  }
}

static std::string offset_plus(std::string_view offs, u32 add) {
  if (add == 0) {
    return std::string(offs);
  }
  return fmt::format("({} + {}u)", offs, add);
}

auto normal_group_load(const ShaderConfig& config, u8 group, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[GX_VA_NRM];
  if (mapping.attrType == GX_NONE || mapping.cnt != 9) {
    return attr_load(config, GX_VA_NRM, vidx);
  }

  auto buf = "vbuf"sv;
  auto offs = fmt::format("ubuf.vtx_start + {} * {}u + {}u", vidx, config.vtxStride, mapping.offset);
  auto le = false; // Vertex buffer is always big endian (for now)
  const bool indexedNbt3 =
      mapping.nrmIndexCount == 3 && (mapping.attrType == GX_INDEX8 || mapping.attrType == GX_INDEX16);
  const u32 groupOffset = indexedNbt3 ? 0 : group * 3u * attr_comp_type_size(mapping.compType);

  if (mapping.attrType == GX_INDEX8) {
    const std::string indexOffs = offset_plus(offs, mapping.nrmIndexCount == 3 ? group : 0);
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS, buf,
                       indexOffs, mapping.stride, groupOffset);
    buf = "abuf"sv;
    le = mapping.le;
  } else if (mapping.attrType == GX_INDEX16) {
    const std::string indexOffs = offset_plus(offs, mapping.nrmIndexCount == 3 ? group * 2u : 0);
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u + {}u", GX_VA_NRM - GX_VA_POS,
                       buf, indexOffs, le, mapping.stride, groupOffset);
    buf = "abuf"sv;
    le = mapping.le;
  } else {
    offs = offset_plus(offs, groupOffset);
  }

  return fetch_attr(mapping, buf, offs, le, 3);
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[attr];
  if (mapping.attrType == GX_NONE) {
    return vtx_attr(config, attr);
  }
  auto buf = "vbuf"sv;
  auto offs = fmt::format("ubuf.vtx_start + {} * {}u + {}u", vidx, config.vtxStride, mapping.offset);
  auto le = false; // Vertex buffer is always big endian (for now)
  if (mapping.attrType == GX_INDEX8) {
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u8_1(&{}, {}) * {}u", attr - GX_VA_POS, buf, offs,
                       mapping.stride);
    buf = "abuf"sv;
    le = mapping.le;
  } else if (mapping.attrType == GX_INDEX16) {
    offs = fmt::format("ubuf.array_start[{}] + raw_fetch_u16_1(&{}, {}, {}) * {}u", attr - GX_VA_POS, buf, offs, le,
                       mapping.stride);
    buf = "abuf"sv;
    le = mapping.le;
  }
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
  case GX_VA_POS: {
    const auto posLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 2) {
      return fmt::format("vec3f({}, 0.0)", posLoad);
    }
    return posLoad;
  }
  case GX_VA_NRM:
    return fetch_attr(mapping, buf, offs, le, mapping.cnt == 9 ? 3 : 0);
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return fetch_color_attr(mapping, buf, offs, le);
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7: {
    const auto texLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 1) {
      return fmt::format("vec2f({}, 0.0)", texLoad);
    }
    return texLoad;
  }
  default:
    Log.fatal("attr_load: Unimplemented {}", attr);
  }
}

auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {
  std::string_view swizzle = alpha ? ".a"sv : ".rgb"sv;
  std::string_view intType = alpha ? "i32"sv : "vec3i"sv;
  std::string_view floatType = alpha ? "f32"sv : "vec3f"sv;
  std::string_view intZero = alpha ? "0"sv : "vec3i(0)"sv;
  std::string_view intMax = alpha ? "255"sv : "vec3i(255)"sv;
  std::string_view shift7 = alpha ? "7u"sv : "vec3u(7u)"sv;
  std::string_view shift8 = alpha ? "8u"sv : "vec3u(8u)"sv;
  std::string outVar;
  std::string_view posVar;
  if (UsePerPixelLighting) {
    outVar = fmt::format("rast{}", i);
    posVar = "in.mv_pos"sv;
  } else {
    outVar = fmt::format("out.cc{}", i);
    posVar = "mv_pos"sv;
  }
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      ambSrc = fmt::format("in.clr{}", i);
    } else {
      ambSrc = vertex_color_attr(config, i);
    }
  } else if (cc.ambSrc == GX_SRC_REG) {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      matSrc = fmt::format("in.clr{}", i);
    } else {
      matSrc = vertex_color_attr(config, i);
    }
  } else if (cc.matSrc == GX_SRC_REG) {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    // The XF lighting block consumes and produces 8-bit channel values even when lighting is disabled.
    if (alpha) {
      return fmt::format("\n    {0}.a = f32(i32(round({1}.a * 255.0))) / 255.0;", outVar, matSrc);
    }
    return fmt::format("\n    {0} = vec4f(vec3f(vec3i(round({1}.rgb * 255.0))) / 255.0, 1.0);", outVar,
                       matSrc);
  }
  GXDiffuseFn diffFn = cc.diffFn;
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    lightAttnFn = fmt::format(R"""(
          var cosine = max(0.0, dot(ldir, light.dir));
          var cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
          var dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
          attn = max(0.0, cos_attn / dist_attn);)""");
  } else if (cc.attnFn == GX_AF_SPEC) {
    std::string_view normal = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
    std::string dist_attn = diffFn != GX_DF_NONE
                                ? "dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn))"
                                : "dot(light.dist_att, vec3f(1.0, attn, attn * attn))";
    lightAttnFn = fmt::format(R"""(
          attn = select(0.0, max(0.0, dot({0}, light.dir)), dot({0}, ldir) >= 0.0);
          var cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
          var dist_attn = {1};
          attn = select(select(0.0, 1.0, cos_attn > 0.0), max(0.0, cos_attn / dist_attn), dist_attn != 0.0);)""",
                              normal, dist_attn);
  }
  std::string_view normal = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
  std::string lightDirSetup;
  if (cc.attnFn == GX_AF_NONE) {
    lightDirSetup = fmt::format(R"""(
          var ldir = light.pos - {0};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = select({1}, ldir / max(dist, 1e-20), dist > 0.0);)""",
                           posVar, normal);
  } else {
    lightDirSetup = fmt::format(R"""(
          var ldir = light.pos - {0};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = ldir / dist;)""",
                           posVar);
  }
  std::string_view lightDiffFn;
  if (diffFn == GX_DF_NONE) {
    lightDiffFn = "1.0"sv;
  } else if (diffFn == GX_DF_SIGN) {
    if (UsePerPixelLighting) {
      lightDiffFn = "dot(ldir, in.mv_nrm)"sv;
    } else {
      lightDiffFn = "dot(ldir, mv_nrm)"sv;
    }
  } else if (diffFn == GX_DF_CLAMP) {
    if (UsePerPixelLighting) {
      lightDiffFn = "max(0.0, dot(ldir, in.mv_nrm))"sv;
    } else {
      lightDiffFn = "max(0.0, dot(ldir, mv_nrm))"sv;
    }
  }
  const auto outputTarget = alpha ? fmt::format("{}.a", outVar) : outVar;
  const auto outputValue =
      alpha
          ? fmt::format("f32((material * (lacc + (lacc >> {}))) >> {}) / 255.0", shift7, shift8)
          : fmt::format("vec4f(vec3f((material * (lacc + (lacc >> {}))) >> {}) / 255.0, 1.0)", shift7,
                        shift8);
  return fmt::format(R"""(
    {{
      var lighting = {11}(round({5}{8} * 255.0));
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];{10}
          var attn: f32;{2}
          var diff = {3};
          lighting = lighting + {11}(round(attn * diff * light.color{8} * 255.0));
      }}
      let lacc = clamp(lighting, {13}, {14});
      let material = {11}(round({4}{8} * 255.0));
      {15} = {16};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv, lightDirSetup, intType, floatType, intZero, intMax, outputTarget,
                     outputValue);
}

wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept {
  ZoneScoped;
  const auto hash = xxh3_hash(config);
  const auto info = build_shader_info(config);

  std::string uniformPre;
  std::string uniBufAttrs;
  std::string texBindings;
  std::string vtxOutAttrs;
  std::string vtxInAttrs;
  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;

  // Load points for line/point expansion
  std::string_view vidxAttr = "vidx"sv;
  if (config.lineMode != 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) iidx: u32";
    uniBufAttrs +=
        "\n    line_width: f32,"
        "\n    line_aspect_y: f32,"
        "\n    line_tex_offset: f32,"
        "\n    line_texcoord_mask: u32,";
    if (config.lineMode == 3) {
      // GX_POINTS: each instance = one vertex, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let in_vidx = iidx;"
          "\n    let in_pos = {};"
          "\n    let in_pnmtxidx = {};"
          "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];",
          attr_load(config, GX_VA_POS, "in_vidx"sv), attr_load(config, GX_VA_PNMTXIDX, "in_vidx"sv));
    } else {
      // GX_LINES / GX_LINESTRIP: each instance = two vertices, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let use_b = vidx >= 2u;"
          "\n    let vidx_a = iidx * {}u;"
          "\n    let vidx_b = vidx_a + 1u;"
          "\n    let in_vidx = select(vidx_a, vidx_b, use_b);"
          "\n    let pos_a = {};"
          "\n    let pos_b = {};"
          "\n    let in_pos = select(pos_a, pos_b, use_b);"
          "\n    let pnmtxidx_a = {};"
          "\n    let pnmtxidx_b = {};"
          "\n    let in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);"
          "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
          "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
          "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);",
          config.lineMode == 1 ? 2 : 1, attr_load(config, GX_VA_POS, "vidx_a"sv),
          attr_load(config, GX_VA_POS, "vidx_b"sv), attr_load(config, GX_VA_PNMTXIDX, "vidx_a"sv),
          attr_load(config, GX_VA_PNMTXIDX, "vidx_b"sv));
    }
    vidxAttr = "in_vidx"sv;
  } else if (config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE) {
    vtxXfrAttrsPre += "\n    let in_pnmtxidx = ubuf.current_pnmtx;";
  }

  // Load vertex attributes
  for (GXAttr attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; attr = static_cast<GXAttr>(attr + 1)) {
    const auto attrType = config.attrs[attr].attrType;
    if (attrType == GX_NONE) {
      continue;
    }
    // in_pnmtxidx and in_pos written above for line mode
    if ((attr != GX_VA_PNMTXIDX && attr != GX_VA_POS) || config.lineMode == 0) {
      vtxXfrAttrsPre += fmt::format("\n    let {} = {};", vtx_attr(config, attr), attr_load(config, attr, vidxAttr));
    }
  }

  if (config.lineMode == 0) {
    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = vec4f({}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
  } else if (config.lineMode == 3) {
    // GX_POINTS: expand single vertex to axis-aligned screen-space square
    vtxXfrAttrsPre +=
        "\n    let clip = vec4f(mv_pos, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let point_size = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let y_sign = select(-1.0, 1.0, vidx >= 2u);"
        "\n    let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);";
  } else {
    // GX_LINES / GX_LINESTRIP: expand line segment perpendicular to direction
    vtxXfrAttrsPre +=
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let ndc_a = clip_a.xy / clip_a.w;"
        "\n    let ndc_b = clip_b.xy / clip_b.w;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_px = vec2f(-dir_px.y, dir_px.x);"
        "\n    let line_width = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);";
  }
  // The near/far depth correction used to be applied here per-vertex (out.pos.z = -out.pos.z for
  // reversed, or += out.pos.w for forward), redundantly on top of the same correction already
  // folded into ubuf.proj by effective_projection() (shader_info.cpp) - applying it twice canceled
  // out for the common case (any draw where effective_projection() decides to flip), silently
  // making "reversed" Z behave identically to forward Z. It is now applied exactly once, in the
  // projection matrix alone (matching upstream aurora commit 1dde08fa: "Move depth correction to
  // projection matrix"), so nothing needs to happen to out.pos.z here.
  // GX rasterizes at a 7/12 pixel center when antialiasing is disabled, while WebGPU rasterizes at 1/2.
  vtxXfrAttrsPre +=
      "\n    let gx_pixel_center_correction = "
      "vec2f(-1.0, 1.0) / (6.0 * max(abs(ubuf.render_viewport_size), vec2f(1.0)));"
      "\n    out.pos = vec4f(out.pos.xy + out.pos.w * gx_pixel_center_correction, out.pos.zw);";
  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = vec4f({}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));

  uniBufAttrs += "\n    proj: mat4x4f,";
  // Only the matrix slots this shader can read are uploaded, in compacted order; see UniformMatrixLayout.
  uniBufAttrs += fmt::format("\n    postex_mtx: array<mat3x4f, {}>,", info.matrixLayout.postexCount);
  uniBufAttrs += fmt::format("\n    nrm_mtx: array<mat3x4f, {}>,", info.matrixLayout.nrmCount);
  std::string fragmentFnPre;
  std::string fragmentFn;
  const int zTexStage = tev_z_texture_stage(config);
  const bool usesZTextureDepth = tev_z_texture_enabled(config);
  std::array<bool, MaxTexCoord> emittedFixedTexCoord{};
  const auto fixed_texcoord_name = [&](u32 texCoordId) {
    if (!emittedFixedTexCoord[texCoordId]) {
      fragmentFnPre += fmt::format(
          "\n    let tex{0}_fixed_uv = vec2i(tex{0}_uv * ubuf.texcoord{0}_scale.xy * 128.0);", texCoordId);
      emittedFixedTexCoord[texCoordId] = true;
    }
    return fmt::format("tex{}_fixed_uv", texCoordId);
  };

  static std::array regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    std::string colorA = color_arg_reg(stage.colorPass.a, idx, config, stage);
    std::string colorB = color_arg_reg(stage.colorPass.b, idx, config, stage);
    fragmentFn += fmt::format("\n    // TEV stage {}", idx);
    if (alpha_compare_uses_color_inputs(stage.alphaOp.op)) {
      const std::string colorAName = fmt::format("tev_stage{}_color_a", idx);
      const std::string colorBName = fmt::format("tev_stage{}_color_b", idx);
      // The color result is emitted before the alpha result below.
      fragmentFn += fmt::format("\n    let {0} = {1};\n    let {2} = {3};", colorAName, colorA, colorBName, colorB);
      colorA = colorAName;
      colorB = colorBName;
    }
    {
      std::string_view outReg = regName[stage.colorOp.outReg];
      std::string op = tev_color_op(
          stage.colorOp.op, stage.colorOp.bias, stage.colorOp.scale, stage.colorOp.clamp, colorA, colorB,
          color_arg_reg(stage.colorPass.c, idx, config, stage), color_arg_reg(stage.colorPass.d, idx, config, stage));
      fragmentFn += fmt::format("\n    {0} = vec4f({1}, {0}.a);", outReg, op);
    }
    {
      std::string_view outReg = regName[stage.alphaOp.outReg];
      std::string op = tev_alpha_op(
          stage.alphaOp.op, stage.alphaOp.bias, stage.alphaOp.scale, stage.alphaOp.clamp,
          alpha_arg_reg(stage.alphaPass.a, idx, config, stage), alpha_arg_reg(stage.alphaPass.b, idx, config, stage),
          alpha_arg_reg(stage.alphaPass.c, idx, config, stage), alpha_arg_reg(stage.alphaPass.d, idx, config, stage),
          colorA, colorB);
      fragmentFn += fmt::format("\n    {0}.a = {1};", outReg, op);
    }
  }

  {
    const auto& lastStage = config.tevStages[config.tevStageCount - 1];
    if (lastStage.colorOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev = vec4f({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
    }
    if (lastStage.alphaOp.outReg != 0) {
      fragmentFn += fmt::format("\n    prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
    }
  }

  const auto loadsTevRegs = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  const auto writesTevRegs = info.writesTevRegRgb | info.writesTevRegAlpha;
  if (loadsTevRegs.test(0)) {
    uniBufAttrs += "\n    tevprev: vec4f,";
    fragmentFnPre += "\n    var prev = ubuf.tevprev;";
  } else {
    fragmentFnPre += "\n    var prev: vec4f;";
  }
  for (int i = 1 /* Skip TEVPREV */; i < loadsTevRegs.size(); ++i) {
    if (loadsTevRegs.test(i)) {
      uniBufAttrs += fmt::format("\n    tevreg{}: vec4f,", i - 1);
      fragmentFnPre += fmt::format("\n    var tevreg{0} = ubuf.tevreg{0};", i - 1);
    } else if (writesTevRegs.test(i)) {
      fragmentFnPre += fmt::format("\n    var tevreg{0}: vec4f;", i - 1);
    }
  }

  if (info.lightingEnabled) {
    uniBufAttrs += fmt::format(FMT_STRING(R"""(
    lights: array<Light, {}>,
    lightState0: u32,
    lightState1: u32,
    lightState0a: u32,
    lightState1a: u32,)"""),
                               GX::MaxLights);
    uniformPre +=
        "\n"
        "struct Light {\n"
        "    pos: vec3f,\n"
        "    dir: vec3f,\n"
        "    color: vec4f,\n"
        "    cos_att: vec3f,\n"
        "    dist_att: vec3f,\n"
        "};";
    if (UsePerPixelLighting) {
      vtxOutAttrs += fmt::format("\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);
      vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
      vtxXfrAttrs += fmt::format(FMT_STRING(R"""(
    out.mv_pos = mv_pos;
    out.mv_nrm = mv_nrm;)"""));
    }
  }

  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }

    const auto& cc = config.colorChannels[i];
    const auto& cca = config.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_amb: vec4f,", i);
    }
    if (cc.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_mat: vec4f,", i);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_amb: vec4f,", i);
    }
    if (cca.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_mat: vec4f,", i);
    }

    // Output vertex color if necessary
    if (UsePerPixelLighting) {
      if ((cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) || cc.matSrc == GX_SRC_VTX ||
          (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX) || cca.matSrc == GX_SRC_VTX) {
        vtxOutAttrs += fmt::format("\n    @location({}) clr{}: vec4f,", vtxOutIdx++, i);
        vtxXfrAttrs += fmt::format("\n    out.clr{} = {};", i, vertex_color_attr(config, i));
      }
    }

    if (UsePerPixelLighting) {
      fragmentFnPre += fmt::format("\n    var rast{}: vec4f;", i);
      fragmentFnPre += lighting_func(config, cc, i, false);
      fragmentFnPre += lighting_func(config, cca, i, true);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) cc{}: vec4f,", vtxOutIdx++, i);
      vtxXfrAttrs += lighting_func(config, cc, i, false);
      vtxXfrAttrs += lighting_func(config, cca, i, true);
      fragmentFnPre += fmt::format("\n    var rast{0} = in.cc{0};", i);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      uniBufAttrs += fmt::format("\n    kcolor{}: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type == GX_TG_MTX3x4) {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uvw: vec3f,", vtxOutIdx++, i);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uv: vec2f,", vtxOutIdx++, i);
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_MAX_TEXGENSRC) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + i)));
    } else if (tcg.src == GX_TG_POS) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_NRM));
    } else if (tcg.src == GX_TG_BINRM) {
      // GX source row 3 is Dolphin's tangent group in NBT normal arrays.
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, normal_group_load(config, 1, vidxAttr));
    } else if (tcg.src == GX_TG_TANGENT) {
      // GX source row 4 is Dolphin's binormal group in NBT normal arrays.
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, normal_group_load(config, 2, vidxAttr));
    } else if (tcg.src == GX_TG_COLOR0) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}.rgb, 1.0);", i, vtx_attr(config, GX_VA_CLR0));
    } else if (tcg.src == GX_TG_COLOR1) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}.rgb, 1.0);", i, vtx_attr(config, GX_VA_CLR1));
    } else
      UNLIKELY FATAL("unhandled tcg src {}", underlying(tcg.src));
    if (tcg.inputFormAB11) {
      vtxXfrAttrs += fmt::format("\n    tc{}.z = 1.0;", i);
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[in_texmtxidx{0} / 3u];", i);
      } else if (tcg.mtx == GX_IDENTITY) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0}.xyz;", i);
      } else {
        const u32 texMtxSlot = (tcg.mtx) / 3;
        const u32 texMtxIdx = texMtxSlot < MaxPostexMtx ? info.matrixLayout.postexRemap[texMtxSlot]
                                                        : texMtxSlot;
        CHECK(texMtxIdx != UniformMatrixLayout::kAbsent,
              "texgen {} matrix slot {} missing from the uniform layout", i, texMtxSlot);
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        vtxXfrAttrs += fmt::format("\n    tc{0}_tmp.z = 1.0f;", i);
      }
    } else if (tcg.type == GX_TG_SRTG) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = vec3f(tc{0}.xy, 1.0f);", i);
    }
    if (config.dualTexEnabled && tcg.normalize) {
      vtxXfrAttrs += fmt::format("\n    tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (!config.dualTexEnabled || tcg.postMtx == GX_PTIDENTITY) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vtxXfrAttrs +=
          fmt::format("\n    var tc{0}_proj = vec4f(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format(
          "\n    if (tc{0}_proj.z == 0.0) {{"
          "\n      tc{0}_proj.x = clamp(tc{0}_proj.x / 2.0, -1.0, 1.0);"
          "\n      tc{0}_proj.y = clamp(tc{0}_proj.y / 2.0, -1.0, 1.0);"
          "\n    }}",
          i);
    }
    // Apply line/point tex offset
    if (config.lineMode == 3) {
      // GX_POINTS: offset S for right columns, T for bottom rows
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u) {{"
          "\n        if ((vidx & 1u) != 0u) {{ tc{0}_proj.x += ubuf.line_tex_offset; }}"
          "\n        if (vidx >= 2u) {{ tc{0}_proj.y += ubuf.line_tex_offset; }}"
          "\n    }}",
          i);
    } else if (config.lineMode != 0) {
      // GX_LINES / GX_LINESTRIP: offset one axis for perpendicular side
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u && (vidx & 1u) != 0u) {{"
          "\n        tc{0}_proj.y += ubuf.line_tex_offset;"
          "\n    }}",
          i);
    }
    if (tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uvw = tc{0}_proj.xyz;", i);
      fragmentFnPre += fmt::format(
          "\n    var tex{0}_uv = in.tex{0}_uvw.xy;"
          "\n    if (in.tex{0}_uvw.z != 0.0) {{"
          "\n      tex{0}_uv = in.tex{0}_uvw.xy / in.tex{0}_uvw.z;"
          "\n    }}",
          i);
    } else {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uv = tc{0}_proj.xy;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
    }
  }
  // Multiple TEV stages may reference the same indirect stage,
  // so we sample each indirect texture only once.
  const auto ind_scale_shift = [](const GXIndTexScale s) -> u32 {
    switch (s) {
    case GX_ITS_1:
      return 0;
    case GX_ITS_2:
      return 1;
    case GX_ITS_4:
      return 2;
    case GX_ITS_8:
      return 3;
    case GX_ITS_16:
      return 4;
    case GX_ITS_32:
      return 5;
    case GX_ITS_64:
      return 6;
    case GX_ITS_128:
      return 7;
    case GX_ITS_256:
      return 8;
    default:
      FATAL("unhandled indirect scale {}", underlying(s));
    }
  };
  for (int i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = config.indStages[i];
    const int effectiveIndTexCoord = tev_effective_texcoord(config, indStage.texCoordId);
    const std::string indFixedUv =
        effectiveIndTexCoord >= 0 ? fixed_texcoord_name(static_cast<u32>(effectiveIndTexCoord)) : "vec2i(0)";
    fragmentFnPre += fmt::format(
        "\n    // Indirect stage {0}"
        "\n    let ind{0}_sample_fixed = {5} >> vec2u({3}u, {4}u);"
        "\n    let t_IndTexCoord{0} = tev_byte_vec3f(textureSampleBias(tex{2}, tex{2}_samp, "
        "vec2f(ind{0}_sample_fixed) / (ubuf.tex{2}_size_bias.xy * 128.0), ubuf.tex{2}_size_bias.z).abg);",
        i, effectiveIndTexCoord, underlying(indStage.texMapId), ind_scale_shift(indStage.scaleS),
        ind_scale_shift(indStage.scaleT), indFixedUv);
  }
  const bool usesFixedTexcoordState = shader_uses_fixed_texcoord_state(config);
  if (usesFixedTexcoordState) {
    fragmentFnPre += "\n    var t_TexCoord = vec2i(0);";
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const auto textureDependency = tev_stage_texture_dependency(config, static_cast<u32>(i));
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool needsTevTexCoord = textureDependency.needsFixedTexcoordState;
    const bool needsTextureSample = textureDependency.combinerUsesTexture || static_cast<int>(i) == zTexStage;
    const bool willSampleTexture = tev_texture_sample_enabled(textureDependency, needsTextureSample);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseCoord = textureDependency.texCoordId >= 0;
    std::string uvIn;
    if (needsTevTexCoord) {
      fragmentFnPre += fmt::format("\n    // TEV stage {} indirect", i);

      // Apply indirect texture matrix (produces an S17.7 fixed-point offset).
      std::string indirectOffsetFixed;
      if (needsIndirectCoord && hasIndirectStage) {
        u32 fmtShift = 0;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtShift = 3;
          break;
        case GX_ITF_4:
          fmtShift = 4;
          break;
        case GX_ITF_3:
          fmtShift = 5;
          break;
        default:
          FATAL("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtShift == 0) {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1} >> vec3u({2}u);", i,
                                       underlying(stage.indTexStage), fmtShift);
        }

        if (stage.indTexBiasSel != GX_ITB_NONE) {
          auto bias = stage.indTexFormat == GX_ITF_8 ? "-128"sv : "1"sv;
          auto biasS = "0"sv, biasT = "0"sv, biasU = "0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1"sv;
          }
          fragmentFnPre += fmt::format("\n    ind{0}_coord = ind{0}_coord + vec3i({1}, {2}, {3}) * {4};", i, biasS,
                                       biasT, biasU, bias);
        }

        const auto appendScaleShift = [&](u32 mtxIdx) {
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_shift = ubuf.ind_mtx[{1}].z;"
              "\n    if (ind{0}_shift >= 0) {{"
              "\n      ind{0}_offset_fixed = ind{0}_offset_fixed >> vec2u(u32(ind{0}_shift));"
              "\n    }} else {{"
              "\n      ind{0}_offset_fixed = ind{0}_offset_fixed << vec2u(u32(-ind{0}_shift));"
              "\n    }}",
              i, mtxIdx * 2 + 1);
          indirectOffsetFixed = fmt::format("ind{}_offset_fixed", i);
        };

        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          // Static 2x3 matrix: integer dot products, then the hardware's fixed >>3 and exponent shift sequence.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_c0 = ubuf.ind_mtx[{1}];"
              "\n    let ind{0}_c1 = ubuf.ind_mtx[{2}];"
              "\n    var ind{0}_offset_fixed = vec2i("
              "\n      (ind{0}_c0.x * ind{0}_coord.x + ind{0}_c0.z * ind{0}_coord.y + "
              "ind{0}_c1.x * ind{0}_coord.z) >> 3u,"
              "\n      (ind{0}_c0.y * ind{0}_coord.x + ind{0}_c0.w * ind{0}_coord.y + "
              "ind{0}_c1.y * ind{0}_coord.z) >> 3u);",
              i, mtxIdx * 2, mtxIdx * 2 + 1);
          appendScaleShift(mtxIdx);
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          // Dynamic S: (fixed UV * indirect S) >> 8, then exponent shift.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          u32 regTexCoord = static_cast<u32>(textureDependency.texCoordId);
          const auto fixedUv = fixed_texcoord_name(regTexCoord);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_offset_fixed = ({1} * vec2i(ind{0}_coord.x)) >> vec2u(8u);", i, fixedUv);
          appendScaleShift(mtxIdx);
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          // Dynamic T: (fixed UV * indirect T) >> 8, then exponent shift.
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          u32 regTexCoord = static_cast<u32>(textureDependency.texCoordId);
          const auto fixedUv = fixed_texcoord_name(regTexCoord);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_offset_fixed = ({1} * vec2i(ind{0}_coord.y)) >> vec2u(8u);", i, fixedUv);
          appendScaleShift(mtxIdx);
        }
      }

      // Wrap base coord and combine with the indirect translation.
      auto wrap_comp = [](GXIndTexWrap wrap, std::string&& coord) -> std::string {
        const int mask = tev_indirect_wrap_mask(wrap);
        if (mask == -1) {
          return std::move(coord);
        }
        if (mask == 0) {
          return "0";
        }
        CHECK(mask > 0, "invalid indirect wrap {}", underlying(wrap));
        return fmt::format("({} & {})", coord, mask);
      };
      std::string baseCoordExpr = "vec2i(0)";
      if (hasBaseCoord) {
        u32 texCoordId = static_cast<u32>(textureDependency.texCoordId);
        baseCoordExpr = fixed_texcoord_name(texCoordId);
      }
      const std::string wrappedExpr =
          fmt::format("vec2i({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                      wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      std::string finalCoord = wrappedExpr;
      if (!indirectOffsetFixed.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetFixed);
      }

      if (usesFixedTexcoordState) {
        if (stage.indTexAddPrev) {
          fragmentFnPre += fmt::format("\n    t_TexCoord += {};", finalCoord);
        } else {
          fragmentFnPre += fmt::format("\n    t_TexCoord = {};", finalCoord);
        }
        fragmentFnPre += "\n    t_TexCoord = tev_s24_fixed_vec2i(t_TexCoord);";

        // ShaderInfo only emits texN_size_bias and texture bindings for a texture that can actually be sampled.
        if (willSampleTexture) {
          u32 texMapId = static_cast<u32>(textureDependency.texMapId);
          fragmentFnPre += fmt::format(
              "\n    var ind{0}_uv = vec2f(t_TexCoord) / (ubuf.tex{1}_size_bias.xy * 128.0);", i, texMapId);
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!willSampleTexture) {
      continue;
    }
    if (uvIn.empty()) {
      // No indirect texturing
      const auto fixedUv = fixed_texcoord_name(static_cast<u32>(textureDependency.texCoordId));
      uvIn = fmt::format("vec2f(tev_s24_fixed_vec2i({0})) / (ubuf.tex{1}_size_bias.xy * 128.0)", fixedUv,
                         textureDependency.texMapId);
    }
    fragmentFnPre +=
        fmt::format("\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,
                    textureDependency.texMapId, uvIn);
  }

  std::string fogDepthExpr = UseReversedZ ? "in.pos.z" : "(1.0 - in.pos.z)";
  std::string fogZCoordExpr =
      fmt::format("u32(round(clamp({}, 0.0, 1.0) * 16777216.0))", fogDepthExpr);
  if (usesZTextureDepth) {
    const u32 zTexBias = config.zTexture & 0x00FFFFFFu;
    const u32 zTexFmt = (config.zTexture >> 24) & 0x3u;
    const u32 zTexOp = (config.zTexture >> 26) & 0x3u;
    const std::string zTexSample = zTexStage >= 0 ? fmt::format("sampled{}", zTexStage) : "vec4f(0.0)";

    fragmentFn += fmt::format(
        "\n    // Z texture"
        "\n    let ztexSample = clamp({0}, vec4f(0.0), vec4f(1.0)) * 255.0;"
        "\n    let ztexRaw = vec4u(round(ztexSample));",
        zTexSample);
    switch (zTexFmt) {
    case 0:
      fragmentFn += fmt::format("\n    var ztexCoord = ({0}u + ztexRaw.a) & 0x00ffffffu;", zTexBias);
      break;
    case 1:
      fragmentFn +=
          fmt::format("\n    var ztexCoord = ({0}u + ((ztexRaw.a << 8u) | ztexRaw.r)) & 0x00ffffffu;", zTexBias);
      break;
    case 2:
    default:
      fragmentFn += fmt::format(
          "\n    var ztexCoord = ({0}u + ((ztexRaw.r << 16u) | (ztexRaw.g << 8u) | ztexRaw.b)) & 0x00ffffffu;",
          zTexBias);
      break;
    }
    if (zTexOp == GX_ZT_ADD) {
      fragmentFn += fmt::format(
          "\n    let oldZ = u32(round(clamp({0}, 0.0, 1.0) * 16777216.0));"
          "\n    ztexCoord = (ztexCoord + oldZ) & 0x00ffffffu;",
          UseReversedZ ? "in.pos.z" : "(1.0 - in.pos.z)");
    }
    fragmentFn += "\n    let ztexDepth = f32(ztexCoord) / 16777216.0;";
    fogZCoordExpr = "ztexCoord";
  }

  if (info.usesPTTexMtx.any())
    uniBufAttrs += fmt::format("\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);
  if (info.usesFog) {
    uniformPre +=
        "\n"
        "struct Fog {\n"
        "    color: vec4f,\n"
        "    a: f32,\n"
        "    b: f32,\n"
        "    c: f32,\n"
        "    pad: f32,\n"
        "    range_base: vec4f,\n"
        "    range_k: array<vec4f, 3>,\n"
        "}";
    uniBufAttrs += "\n    fog: Fog,";

    if ((config.fogType & 0x8u) == 0) {
      fragmentFn += fmt::format(
          "\n    // Fog"
          "\n    let fogZCoord = {};"
          "\n    let fogShiftedZ = fogZCoord >> u32(ubuf.fog.pad);"
          "\n    var fogZe = (ubuf.fog.a * 16777216.0) / (ubuf.fog.b - f32(fogShiftedZ));",
          fogZCoordExpr);
    } else {
      fragmentFn += fmt::format(
          "\n    // Fog"
          "\n    let fogZCoord = {};"
          "\n    var fogZe = ubuf.fog.a * f32(fogZCoord) / 16777216.0;",
          fogZCoordExpr);
    }
    if (config.fogRangeAdjust) {
      fragmentFn +=
          "\n    let fogRangeOffset = (2.0 * (in.pos.x / ubuf.fog.range_base.y)) - 1.0 - ubuf.fog.range_base.x;"
          "\n    let fogRangeFloatIndex = clamp(9.0 - abs(fogRangeOffset) * 9.0, 0.0, 9.0);"
          "\n    let fogRangeIndexLower = u32(fogRangeFloatIndex);"
          "\n    let fogRangeIndexUpper = fogRangeIndexLower + 1u;"
          "\n    let fogRangeKLower = ubuf.fog.range_k[fogRangeIndexLower >> 2u][fogRangeIndexLower & 3u];"
          "\n    let fogRangeKUpper = ubuf.fog.range_k[fogRangeIndexUpper >> 2u][fogRangeIndexUpper & 3u];"
          "\n    let fogRangeK = mix(fogRangeKLower, fogRangeKUpper, fract(fogRangeFloatIndex));"
          "\n    fogZe *= sqrt(fogRangeOffset * fogRangeOffset + fogRangeK * fogRangeK) / fogRangeK;";
    }
    fragmentFn += "\n    var fogF = clamp(fogZe - ubuf.fog.c, 0.0, 1.0);";
    switch (config.fogType) {
      DEFAULT_FATAL("invalid fog type {}", config.fogType);
    case static_cast<GXFogType>(1):
    case static_cast<GXFogType>(3):
    case GX_FOG_PERSP_LIN:
    case static_cast<GXFogType>(9):
    case static_cast<GXFogType>(11):
    case GX_FOG_ORTHO_LIN:
      fragmentFn += "\n    var fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fragmentFn += "\n    var fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fragmentFn +=
          "\n    fogF = 1.0 - fogF;"
          "\n    var fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    }
    fragmentFn += "\n    prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";
  }
  if (info.usedIndTexMtxs.any()) {
    uniBufAttrs += "\n    ind_mtx: array<vec4i, 6>,";
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (info.sampledTexCoords.test(i)) {
      uniBufAttrs += fmt::format("\n    texcoord{}_scale: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    uniBufAttrs += fmt::format("\n    tex{}_size_bias: vec4f,", i);
    texBindings += fmt::format(
        "\n@group(2) @binding({1})\n"
        "var tex{0}: texture_2d<f32>;\n"
        "@group(2) @binding({2})\n"
        "var tex{0}_samp: sampler;",
        i, i * 2, i * 2 + 1);
  }
  fragmentFn += "\n    prev = tev_overflow_vec4f(prev);";
  if (config.alphaCompare) {
    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (config.alphaCompare.op) {
    default:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fragmentFn += "\n    // Alpha compare\n    discard;";
    } else if (discard.constant != 0) {
      fragmentFn += "\n    // Alpha compare"
                    "\n    let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));";
      fragmentFn += fmt::format("\n    if ({}) {{ discard; }}", discard.expr);
    }
  }

  std::string fragmentReturnType = "@location(0) vec4f";
  std::string fragmentReturn = "    return prev;";
  if (usesZTextureDepth) {
    uniformPre +=
        "\n"
        "struct FragmentOutput {\n"
        "    @location(0) color: vec4f,\n"
        "    @builtin(frag_depth) depth: f32,\n"
        "};";

    fragmentFn += fmt::format("\n    let fragDepth = {}ztexDepth;", UseReversedZ ? "" : "1.0 - ");
    fragmentReturnType = "FragmentOutput";
    fragmentReturn =
        "    var out: FragmentOutput;\n"
        "    out.color = prev;\n"
        "    out.depth = fragDepth;\n"
        "    return out;";
  }

  const auto shaderSource = fmt::format(R"""(
fn bswap32(v: u32, le: bool) -> u32 {{
  if (le) {{
    return v;
  }}
  return ((v & 0x000000FFu) << 24u) |
         ((v & 0x0000FF00u) << 8u) |
         ((v & 0x00FF0000u) >> 8u) |
         ((v & 0xFF000000u) >> 24u);
}}

fn bswap16(v: u32, le: bool) -> u32 {{
  return select(((v & 0xFFu) << 8u) | (v >> 8u), v, le);
}}

fn load_u8(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word = p[byte_off / 4u];
  let shift = (byte_off & 3u) * 8u;
  return (word >> shift) & 0xFFu;
}}

fn load_u32_raw(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let lo = p[word_idx];
  if (sub == 0u) {{
    return lo;
  }}
  let hi = p[word_idx + 1u];
  let shift = sub * 8u;
  return (lo >> shift) | (hi << (32u - shift));
}}

fn load_u16(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = p[word_idx];
  if (sub <= 2u) {{
    return bswap16(extractBits(word, sub * 8u, 16u), le);
  }}
  let next = p[word_idx + 1u];
  let raw = extractBits(word, 24u, 8u) | (extractBits(next, 0u, 8u) << 8u);
  return bswap16(raw, le);
}}

fn load_u24(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let raw = load_u32_raw(p, byte_off) & 0x00FFFFFFu;
  if (le) {{
    return raw;
  }}
  return ((raw & 0x0000FFu) << 16u) |
         (raw & 0x00FF00u) |
         ((raw & 0xFF0000u) >> 16u);
}}

fn load_u32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return bswap32(load_u32_raw(p, byte_off), le);
}}

fn load_f32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return bitcast<f32>(load_u32(p, byte_off, le));
}}

fn raw_fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  return load_u8(p, byte_off);
}}

fn raw_fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32) -> vec2u {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = p[word_idx];
  if (sub <= 2u) {{
    let shift = sub * 8u;
    return vec2u(
      extractBits(word, shift + 0u, 8u),
      extractBits(word, shift + 8u, 8u),
    );
  }}
  let next = p[word_idx + 1u];
  return vec2u(
    extractBits(word, 24u, 8u),
    extractBits(next, 0u, 8u),
  );
}}

fn raw_fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32) -> vec3u {{
  let raw = load_u32_raw(p, byte_off);
  return vec3u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
  );
}}

fn raw_fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32) -> vec4u {{
  let raw = load_u32_raw(p, byte_off);
  return vec4u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
    extractBits(raw, 24u, 8u),
  );
}}

fn raw_fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return load_u16(p, byte_off, le);
}}

fn raw_fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2u {{
  return vec2u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
  );
}}

fn raw_fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3u {{
  return vec3u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4u {{
  return vec4u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
    load_u16(p, byte_off + 6u, le),
  );
}}

fn raw_fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return load_f32(p, byte_off, le);
}}

fn raw_fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return vec2f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return vec3f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
  );
}}

fn raw_fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return vec4f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
    load_f32(p, byte_off + 12u, le),
  );
}}

fn fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u8_1(p, byte_off);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = (bitcast<i32>(raw_fetch_u8_1(p, byte_off)) << 24) >> 24;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u8_2(p, byte_off);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u8_2(p, byte_off)) << vec2u(24u)) >> vec2u(24u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u8_3(p, byte_off)) << vec3u(24u)) >> vec3u(24u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u8_4(p, byte_off)) << vec4u(24u)) >> vec4u(24u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u16_1(p, byte_off, le);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = bitcast<i32>(raw_fetch_u16_1(p, byte_off, le) << 16u) >> 16;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u16_2(p, byte_off, le);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u16_2(p, byte_off, le)) << vec2u(16u)) >> vec2u(16u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u16_3(p, byte_off, le);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u16_3(p, byte_off, le)) << vec3u(16u)) >> vec3u(16u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u16_4(p, byte_off, le);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u16_4(p, byte_off, le)) << vec4u(16u)) >> vec4u(16u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return raw_fetch_f32_1(p, byte_off, le);
}}

fn fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return raw_fetch_f32_2(p, byte_off, le);
}}

fn fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return raw_fetch_f32_3(p, byte_off, le);
}}

fn fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return raw_fetch_f32_4(p, byte_off, le);
}}

fn fetch_rgb565(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 11u) & 0x1Fu) / f32(0x1Fu),
    f32((v >>  5u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x1Fu) / f32(0x1Fu),
    1.0,
  );
}}

fn fetch_rgb8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgbx8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgba4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 12u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  8u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  4u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  0u) & 0x0Fu) / f32(0x0Fu),
  );
}}

fn fetch_rgba6(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u24(p, byte_off, le);
  return vec4f(
    f32((v >> 18u) & 0x3Fu) / f32(0x3Fu),
    f32((v >> 12u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  6u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x3Fu) / f32(0x3Fu),
  );
}}

fn fetch_rgba8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / 255.0;
}}

fn tev_overflow_f32(in: f32) -> f32 {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec3f(in: vec3f) -> vec3f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec4f(in: vec4f) -> vec4f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_s24_fixed_vec2i(in: vec2i) -> vec2i {{
  return (in << vec2u(8u)) >> vec2u(8u);
}}

fn tev_byte_f32(in: f32) -> i32 {{
  return i32(round(in * 255.0));
}}

fn tev_byte_vec3f(in: vec3f) -> vec3i {{
  return vec3i(round(in * 255.0));
}}

fn tev_regular_i32(a: i32, b: i32, c: i32, d: i32, bias: i32, scale: u32, is_sub: bool) -> i32 {{
  var d_part = d + bias;
  var lerp_part = (a << 8u) + (b - a) * (c + (c >> 7u));
  if (scale == 1u) {{
    d_part = d_part << 1u;
    lerp_part = lerp_part << 1u;
  }}
  if (scale == 2u) {{
    d_part = d_part << 2u;
    lerp_part = lerp_part << 2u;
  }}
  if (scale != 3u) {{
    lerp_part += select(128, 127, is_sub);
  }}
  let lerp = lerp_part >> 8u;
  var result = select(d_part + lerp, d_part - lerp, is_sub);
  if (scale == 3u) {{
    result = result >> 1u;
  }}
  return result;
}}

fn tev_regular_f32(a: f32, b: f32, c: f32, d: f32, bias: i32, scale: u32, is_sub: bool) -> f32 {{
  return f32(tev_regular_i32(tev_byte_f32(a), tev_byte_f32(b), tev_byte_f32(c), tev_byte_f32(d), bias, scale, is_sub)) /
         255.0;
}}

fn tev_regular_vec3f(a: vec3f, b: vec3f, c: vec3f, d: vec3f, bias: i32, scale: u32, is_sub: bool) -> vec3f {{
  let ai = tev_byte_vec3f(a);
  let bi = tev_byte_vec3f(b);
  let ci = tev_byte_vec3f(c);
  let di = tev_byte_vec3f(d);
  return vec3f(
    f32(tev_regular_i32(ai.x, bi.x, ci.x, di.x, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.y, bi.y, ci.y, di.y, bias, scale, is_sub)) / 255.0,
    f32(tev_regular_i32(ai.z, bi.z, ci.z, di.z, bias, scale, is_sub)) / 255.0,
  );
}}

{8}

struct Uniform {{
    vtx_start: u32,
    current_pnmtx: u32,
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    pad: vec2u,
    array_start: array<u32, 12>,{0}
}};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;{1}

struct VertexOutput {{
    @builtin(position) pos: vec4f,{2}
}};

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32{3}
) -> VertexOutput {{
    var out: VertexOutput;{7}{4}
    return out;
}}

@fragment
fn fs_main(in: VertexOutput) -> {9} {{{6}{5}
{10}
}}
)""",
                                        uniBufAttrs, texBindings, vtxOutAttrs, vtxInAttrs, vtxXfrAttrs, fragmentFn,
                                        fragmentFnPre, vtxXfrAttrsPre, uniformPre, fragmentReturnType,
                                        fragmentReturn);
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = shaderSource.c_str();
  const auto label = fmt::format("GX Shader {:x}", hash);
  const auto shaderDescriptor = wgpu::ShaderModuleDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  return webgpu::g_device.CreateShaderModule(&shaderDescriptor);
}
} // namespace aurora::gx
