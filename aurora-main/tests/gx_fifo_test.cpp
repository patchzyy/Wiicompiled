// GX FIFO encode/decode round-trip tests: call a GX function, capture the FIFO bytes, reset
// g_gxState, feed them to command_processor::process(), then check the decoded state.

#include "gx_test_common.hpp"
#include "gfx/efb_ram_encoder.hpp"
#include "gfx/tex_copy_format_contract.hpp"
#include "gx/shader_info.hpp"
#include "gx/pipeline.hpp"
#include "__gx.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

using aurora::gx::g_gxState;

TEST(GXPipelineConfig, RejectsInvalidDeserializedEnums) {
  aurora::gx::PipelineConfig config{};
  EXPECT_TRUE(aurora::gx::valid_pipeline_config(config));

  config.depthFunc = static_cast<GXCompare>(0x009F01F2);
  EXPECT_FALSE(aurora::gx::valid_pipeline_config(config));
}

TEST(GXPipelineConfig, FoggedLateZLogicOrPreservesEggDofMask) {
  EXPECT_EQ(aurora::gx::effective_pipeline_fog_type(GX_FOG_PERSP_LIN, GX_ZT_REPLACE,
                                                    false, GX_BM_LOGIC, GX_LO_OR),
            GX_FOG_PERSP_LIN);

  // Keep the existing defensive suppression for late-Z mask passes whose
  // integer logic operation still has only an inexact WebGPU fallback.
  EXPECT_EQ(aurora::gx::effective_pipeline_fog_type(GX_FOG_PERSP_LIN, GX_ZT_REPLACE,
                                                    false, GX_BM_LOGIC, GX_LO_COPY),
            GX_FOG_NONE);

  // Logic fallbacks do not require this workaround when Z-texturing is early.
  EXPECT_EQ(aurora::gx::effective_pipeline_fog_type(GX_FOG_PERSP_LIN, GX_ZT_REPLACE,
                                                    true, GX_BM_LOGIC, GX_LO_COPY),
            GX_FOG_PERSP_LIN);
}

TEST(GXShaderInfo, ShaderLightSanitizesNonFiniteDirectionForUniforms) {
  aurora::gx::Light light{};
  light.dir = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
  };

  const auto sanitized = aurora::gx::prepare_shader_light(light);
  EXPECT_FLOAT_EQ(sanitized.dir[0], 0.0f);
  EXPECT_FLOAT_EQ(sanitized.dir[1], 0.0f);
  EXPECT_FLOAT_EQ(sanitized.dir[2], 0.0f);

  light.dir = {3.0f, 4.0f, 0.0f};
  const auto normalized = aurora::gx::prepare_shader_light(light);
  EXPECT_FLOAT_EQ(normalized.dir[0], 0.6f);
  EXPECT_FLOAT_EQ(normalized.dir[1], 0.8f);
  EXPECT_FLOAT_EQ(normalized.dir[2], 0.0f);

  light.dir = {0.0f, 0.0f, 0.0f};
  const auto zero = aurora::gx::prepare_shader_light(light);
  EXPECT_FLOAT_EQ(zero.dir[0], 0.0f);
  EXPECT_FLOAT_EQ(zero.dir[1], 0.0f);
  EXPECT_FLOAT_EQ(zero.dir[2], 0.0f);
}

TEST(GXLighting, SpotCoefficientsAndPositionGetterMatchRevolutionSdk) {
  GXLightObj light{};
  GXInitLightSpot(&light, 60.0f, GX_SP_SHARP);

  float a0 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  GXGetLightAttnA(&light, &a0, &a1, &a2);
  EXPECT_NEAR(a0, -3.0f, 0.00001f);
  EXPECT_NEAR(a1, 8.0f, 0.00001f);
  EXPECT_NEAR(a2, -4.0f, 0.00001f);

  GXInitLightSpot(&light, 60.0f, GX_SP_RING1);
  GXGetLightAttnA(&light, &a0, &a1, &a2);
  EXPECT_NEAR(a0, -8.0f, 0.00001f);
  EXPECT_NEAR(a1, 24.0f, 0.00001f);
  EXPECT_NEAR(a2, -16.0f, 0.00001f);

  GXInitLightPos(&light, 10.0f, 20.0f, 30.0f);
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  GXGetLightPos(&light, &x, &y, &z);
  EXPECT_FLOAT_EQ(x, 10.0f);
  EXPECT_FLOAT_EQ(y, 20.0f);
  EXPECT_FLOAT_EQ(z, 30.0f);
}

TEST(EfbRamEncoderContract, Z24X8WritesNativeFourByFourArGbPlanes) {
  std::array<u8, 4 * 4 * 4> rgba{};
  for (u32 i = 0; i < 16; ++i) {
    rgba[i * 4 + 0] = static_cast<u8>(0x10 + i); // high Z
    rgba[i * 4 + 1] = static_cast<u8>(0x40 + i); // middle Z
    rgba[i * 4 + 2] = static_cast<u8>(0x80 + i); // low Z
    rgba[i * 4 + 3] = 0xff;
  }

  std::array<u8, 64> encoded{};
  ASSERT_TRUE(aurora::gfx::efb_ram::encode(
      encoded.data(), encoded.size(), GX_TF_Z24X8, 4, 4, rgba.data(), 4, 4, 16,
      aurora::gfx::efb_ram::HostPixelOrder::RGBA));
  EXPECT_EQ(aurora::gfx::efb_ram::encoded_size(GX_TF_Z24X8, 4, 4), encoded.size());

  for (u32 i = 0; i < 16; ++i) {
    EXPECT_EQ(encoded[i * 2 + 0], 0xff) << "AR plane texel " << i;
    EXPECT_EQ(encoded[i * 2 + 1], 0x10 + i) << "AR plane texel " << i;
    EXPECT_EQ(encoded[32 + i * 2 + 0], 0x40 + i) << "GB plane texel " << i;
    EXPECT_EQ(encoded[32 + i * 2 + 1], 0x80 + i) << "GB plane texel " << i;

    const u32 depth = (static_cast<u32>(encoded[i * 2 + 1]) << 16) |
                      (static_cast<u32>(encoded[32 + i * 2 + 0]) << 8) |
                      encoded[32 + i * 2 + 1];
    EXPECT_EQ(depth, ((0x10u + i) << 16) | ((0x40u + i) << 8) | (0x80u + i));
  }
}

static bool has_bp_write(const std::vector<u8>& bytes, u8 reg) {
  const std::array<u8, 2> pattern{0x61, reg};
  return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

static void expect_fog_raw_fields_match_decoded_state() {
  const float expectedA = std::ldexp(g_gxState.fog.aRaw, static_cast<int>(g_gxState.fog.bShift));
  const float bMant = static_cast<float>(g_gxState.fog.bMagnitude) / 8388638.0f;
  const float expectedB = std::ldexp(bMant, static_cast<int>(g_gxState.fog.bShift) - 1);
  EXPECT_NEAR(g_gxState.fog.a, expectedA, std::max(std::abs(g_gxState.fog.a) * 1e-3f, 1e-6f));
  EXPECT_NEAR(g_gxState.fog.b, expectedB, std::max(std::abs(g_gxState.fog.b) * 1e-3f, 1e-6f));
}

static bool has_aurora_cmd(const std::vector<u8>& bytes, u16 cmd) {
  const std::array<u8, 3> pattern{GX_LOAD_AURORA, static_cast<u8>(cmd >> 8), static_cast<u8>(cmd & 0xFF)};
  return std::search(bytes.begin(), bytes.end(), pattern.begin(), pattern.end()) != bytes.end();
}

static std::vector<u8> bp_cmd(u8 reg, u32 value) {
  return {0x61, reg, static_cast<u8>((value >> 16) & 0xFF), static_cast<u8>((value >> 8) & 0xFF),
          static_cast<u8>(value & 0xFF)};
}

static std::vector<u8> cp_cmd(u8 reg, u32 value) {
  return {0x08, reg, static_cast<u8>((value >> 24) & 0xFF), static_cast<u8>((value >> 16) & 0xFF),
          static_cast<u8>((value >> 8) & 0xFF), static_cast<u8>(value & 0xFF)};
}

static std::vector<u8> xf_cmd(u16 addr, std::initializer_list<u32> values) {
  std::vector<u8> bytes;
  bytes.reserve(5 + values.size() * 4);
  bytes.push_back(0x10);

  const u32 header = ((static_cast<u32>(values.size() - 1) & 0xFFFFu) << 16) | addr;
  bytes.push_back(static_cast<u8>((header >> 24) & 0xFF));
  bytes.push_back(static_cast<u8>((header >> 16) & 0xFF));
  bytes.push_back(static_cast<u8>((header >> 8) & 0xFF));
  bytes.push_back(static_cast<u8>(header & 0xFF));

  for (const u32 value : values) {
    bytes.push_back(static_cast<u8>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<u8>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<u8>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<u8>(value & 0xFF));
  }
  return bytes;
}

static u32 read_be32_at(const std::vector<u8>& bytes, size_t offset) {
  return (static_cast<u32>(bytes[offset]) << 24) | (static_cast<u32>(bytes[offset + 1]) << 16) |
         (static_cast<u32>(bytes[offset + 2]) << 8) | static_cast<u32>(bytes[offset + 3]);
}

TEST(FrameInterpolationContract, RequiresStablePerspectiveDrawSequence) {
  const auto resetInterpolation = [] {
    aurora::gx::set_frame_interpolation_fps(0);
    aurora::gx::begin_frame_interpolation();
  };
  const auto buildFrame = [](const aurora::gx::ShaderInfo& info, aurora::HashType signatureBase,
                             bool perspective, bool reverse = false) {
    aurora::gx::begin_frame_interpolation();
    aurora::gx::BindGroupRanges ranges{};
    uint32_t interpolatedUniforms = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t signatureOffset = reverse ? 7 - i : i;
      const aurora::gx::FrameInterpolationDrawIdentity identity{
          .combined = signatureBase + signatureOffset,
          .pipeline = signatureBase,
          .texture = 1,
      };
      const auto uniforms = aurora::gx::build_uniform(info, 0, ranges, identity, perspective);
      interpolatedUniforms += static_cast<uint32_t>(std::count_if(
          uniforms.interpolated.begin(), uniforms.interpolated.end(),
          [](const aurora::gfx::Range& range) { return range.size != 0; }));
    }
    aurora::gx::finalize_frame_interpolation();
    return interpolatedUniforms;
  };

  resetInterpolation();
  aurora::gx::set_frame_interpolation_fps(120);
  const auto info = aurora::gx::build_shader_info({});

  // Slots are inserted whenever interpolation is configured, so a frame with no matches just fills
  // them with duplicates. The staged uniform counts below are what verify the matching.
  EXPECT_EQ(buildFrame(info, 100, true), 0u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());

  EXPECT_EQ(buildFrame(info, 100, true), 8u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());
  EXPECT_EQ(aurora::gx::interpolated_frame_count(), 1u);
  EXPECT_TRUE(aurora::gx::frame_interpolation_replay_safe());

  EXPECT_EQ(buildFrame(info, 100, true, true), 8u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());

  aurora::gx::mark_frame_interpolation_replay_unsafe();
  EXPECT_FALSE(aurora::gx::frame_interpolation_replay_safe());
  EXPECT_EQ(aurora::gx::interpolated_frame_count(), 1u);

  aurora::gx::set_frame_interpolation_fps(180);
  EXPECT_EQ(buildFrame(info, 250, true), 0u);
  EXPECT_TRUE(aurora::gx::frame_interpolation_replay_safe());
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());
  EXPECT_EQ(buildFrame(info, 250, true), 16u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());
  EXPECT_EQ(aurora::gx::interpolated_frame_count(), 2u);

  aurora::gx::set_frame_interpolation_fps(120);
  EXPECT_EQ(buildFrame(info, 200, true), 0u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());

  EXPECT_EQ(buildFrame(info, 200, false), 0u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());

  aurora::gx::set_frame_interpolation_fps(240);
  EXPECT_EQ(buildFrame(info, 300, true), 0u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());
  EXPECT_EQ(buildFrame(info, 300, true), 24u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());
  EXPECT_EQ(aurora::gx::interpolated_frame_count(), 3u);

  // CPU-deformed draws change their vertex bytes every frame, so their stable pipeline and texture
  // identity still has to retain transform history for the camera and model matrices.
  aurora::gx::set_frame_interpolation_fps(120);
  const auto buildDeformedFrame = [&](aurora::HashType geometryEpoch) {
    aurora::gx::begin_frame_interpolation();
    aurora::gx::BindGroupRanges ranges{};
    uint32_t interpolatedUniforms = 0;
    for (uint32_t i = 0; i < 8; ++i) {
      const aurora::gx::FrameInterpolationDrawIdentity identity{
          .combined = geometryEpoch + i,
          .pipeline = 42,
          .texture = 7,
      };
      const auto uniforms = aurora::gx::build_uniform(info, 0, ranges, identity, true);
      interpolatedUniforms += static_cast<uint32_t>(std::count_if(
          uniforms.interpolated.begin(), uniforms.interpolated.end(),
          [](const aurora::gfx::Range& range) { return range.size != 0; }));
    }
    aurora::gx::finalize_frame_interpolation();
    return interpolatedUniforms;
  };
  EXPECT_EQ(buildDeformedFrame(400), 0u);
  EXPECT_EQ(buildDeformedFrame(500), 8u);
  EXPECT_TRUE(aurora::gx::has_interpolated_frame());

  resetInterpolation();
}

TEST(FrameInterpolationContract, DecomposesRotationScaleAndTranslation) {
  const aurora::Mat3x4<float> previous{
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  const aurora::Mat3x4<float> current{
      {0.0f, -2.0f, 0.0f, 10.0f},
      {2.0f, 0.0f, 0.0f, 20.0f},
      {0.0f, 0.0f, 2.0f, 30.0f},
  };
  aurora::Mat3x4<float> midpoint{};
  ASSERT_TRUE(aurora::gx::interpolate_transform_midpoint(previous, current, midpoint));

  const float expectedAxis = std::sqrt(0.5f) * 1.5f;
  EXPECT_NEAR(midpoint.m0.x(), expectedAxis, 1.0e-5f);
  EXPECT_NEAR(midpoint.m0.y(), -expectedAxis, 1.0e-5f);
  EXPECT_NEAR(midpoint.m1.x(), expectedAxis, 1.0e-5f);
  EXPECT_NEAR(midpoint.m1.y(), expectedAxis, 1.0e-5f);
  EXPECT_NEAR(midpoint.m2.z(), 1.5f, 1.0e-5f);
  EXPECT_FLOAT_EQ(midpoint.m0.w(), 5.0f);
  EXPECT_FLOAT_EQ(midpoint.m1.w(), 10.0f);
  EXPECT_FLOAT_EQ(midpoint.m2.w(), 15.0f);

  aurora::Mat3x4<float> quarter{};
  aurora::Mat3x4<float> threeQuarter{};
  ASSERT_TRUE(aurora::gx::interpolate_transform(previous, current, 0.25f, quarter));
  ASSERT_TRUE(aurora::gx::interpolate_transform(previous, current, 0.75f, threeQuarter));
  EXPECT_FLOAT_EQ(quarter.m0.w(), 2.5f);
  EXPECT_FLOAT_EQ(quarter.m1.w(), 5.0f);
  EXPECT_FLOAT_EQ(quarter.m2.w(), 7.5f);
  EXPECT_FLOAT_EQ(threeQuarter.m0.w(), 7.5f);
  EXPECT_FLOAT_EQ(threeQuarter.m1.w(), 15.0f);
  EXPECT_FLOAT_EQ(threeQuarter.m2.w(), 22.5f);
}

TEST(FrameInterpolationContract, IndexedPaletteInterpolationPreservesSharedSeams) {
  const aurora::Mat3x4<float> identity{
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  const aurora::Mat3x4<float> quarterTurn{
      {0.0f, -1.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  const aurora::Mat3x4<float> translatedPrevious{
      {1.0f, 0.0f, 0.0f, 1.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  const aurora::Mat3x4<float> translatedCurrent{
      {1.0f, 0.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };

  aurora::Mat3x4<float> rotatedMidpoint{};
  aurora::Mat3x4<float> translatedMidpoint{};
  ASSERT_TRUE(aurora::gx::interpolate_indexed_transform(
      identity, quarterTurn, 0.5f, rotatedMidpoint));
  ASSERT_TRUE(aurora::gx::interpolate_indexed_transform(
      translatedPrevious, translatedCurrent, 0.5f, translatedMidpoint));

  const auto transformPoint = [](const aurora::Mat3x4<float>& matrix,
                                 const std::array<float, 3>& point) {
    return std::array<float, 3>{
        matrix.m0.x() * point[0] + matrix.m0.y() * point[1] +
            matrix.m0.z() * point[2] + matrix.m0.w(),
        matrix.m1.x() * point[0] + matrix.m1.y() * point[1] +
            matrix.m1.z() * point[2] + matrix.m1.w(),
        matrix.m2.x() * point[0] + matrix.m2.y() * point[1] +
            matrix.m2.z() * point[2] + matrix.m2.w(),
    };
  };

  // Two representations of the same seam point that agree at both real frames, so coefficient
  // interpolation has to keep them coincident; a rigid decompose would pull them apart.
  const auto rotatedSeam = transformPoint(rotatedMidpoint, {1.0f, 0.0f, 0.0f});
  const auto translatedSeam = transformPoint(translatedMidpoint, {0.0f, 0.0f, 0.0f});
  for (size_t component = 0; component < rotatedSeam.size(); ++component) {
    EXPECT_FLOAT_EQ(rotatedSeam[component], translatedSeam[component]);
  }
  EXPECT_FLOAT_EQ(rotatedSeam[0], 0.5f);
  EXPECT_FLOAT_EQ(rotatedSeam[1], 0.5f);

  // Composed palette matrices may legitimately contain shear, so the indexed path interpolates it
  // instead of rejecting the matrix and snapping those vertices to the new frame.
  const aurora::Mat3x4<float> sheared{
      {1.0f, 0.5f, 0.0f, 4.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 0.0f},
  };
  aurora::Mat3x4<float> shearedMidpoint{};
  ASSERT_TRUE(aurora::gx::interpolate_indexed_transform(
      identity, sheared, 0.5f, shearedMidpoint));
  EXPECT_FLOAT_EQ(shearedMidpoint.m0.y(), 0.25f);
  EXPECT_FLOAT_EQ(shearedMidpoint.m0.w(), 2.0f);
}

TEST(FrameInterpolationContract, IndexedPaletteHistoryKeepsAbsoluteVertexSlots) {
  constexpr uint16_t usedMask = (1u << 0) | (1u << 1);
  constexpr size_t projectionOffset = 0;
  constexpr size_t positionOffset = sizeof(aurora::Mat4x4<float>);
  constexpr size_t normalOffset =
      positionOffset + aurora::gx::MaxPnMtx * sizeof(aurora::Mat3x4<float>);
  constexpr size_t uniformSize =
      normalOffset + aurora::gx::MaxPnMtx * sizeof(aurora::Mat3x4<float>);
  const aurora::gx::FrameInterpolationDrawIdentity identity{
      .combined = 0x1234,
      .pipeline = 0x5678,
      .texture = 0x9abc,
      .matrixTopology = 0xdef0,
  };
  const aurora::Mat4x4<float> projection{};
  const auto matrixAt = [](float x) {
    return aurora::Mat3x4<float>{
        {1.0f, 0.0f, 0.0f, x},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
  };
  const auto recordFrame = [&](const aurora::gx::FrameInterpolationDrawIdentity& drawIdentity,
                               float slot0X, float slot1X,
                               std::array<uint8_t, uniformSize>& source) {
    g_gxState.pnMtx[0].pos = matrixAt(slot0X);
    g_gxState.pnMtx[1].pos = matrixAt(slot1X);
    g_gxState.pnMtx[0].nrm = matrixAt(0.0f);
    g_gxState.pnMtx[1].nrm = matrixAt(0.0f);
    std::memcpy(source.data() + positionOffset, &g_gxState.pnMtx[0].pos,
                sizeof(aurora::Mat3x4<float>));
    std::memcpy(source.data() + positionOffset + sizeof(aurora::Mat3x4<float>),
                &g_gxState.pnMtx[1].pos, sizeof(aurora::Mat3x4<float>));
    std::memcpy(source.data() + normalOffset, &g_gxState.pnMtx[0].nrm,
                sizeof(aurora::Mat3x4<float>));
    std::memcpy(source.data() + normalOffset + sizeof(aurora::Mat3x4<float>),
                &g_gxState.pnMtx[1].nrm, sizeof(aurora::Mat3x4<float>));
    return aurora::gx::record_interpolation_draw(
        drawIdentity, projection, usedMask,
        aurora::gx::InterpolatedUniformLayout{
            .sourceUniformData = source.data(),
            .uniformSize = source.size(),
            .projectionOffset = projectionOffset,
            .positionOffset = positionOffset,
            .normalOffset = normalOffset,
            .currentMatrix = 0,
            .indexedMatrices = true,
        });
  };

  aurora::gx::set_frame_interpolation_fps(0);
  aurora::gx::begin_frame_interpolation();
  aurora::gx::set_frame_interpolation_fps(120);

  std::array<uint8_t, uniformSize> previousSource{};
  aurora::gx::begin_frame_interpolation();
  recordFrame(identity, 0.0f, 100.0f, previousSource);
  aurora::gx::finalize_frame_interpolation();

  // Current slot 0 is spatially nearest previous slot 1 and vice versa. The old nearest-unused
  // heuristic crossed them even though vertex PNMTXIDX bytes address absolute slots 0 and 1.
  aurora::gfx::testing::reset_uniform_allocations();
  std::array<uint8_t, uniformSize> currentSource{};
  aurora::gx::begin_frame_interpolation();
  const auto ranges = recordFrame(identity, 90.0f, 10.0f, currentSource);
  ASSERT_NE(ranges[0].size, 0u);
  aurora::gx::finalize_frame_interpolation();

  const auto& interpolated = aurora::gfx::testing::uniform_allocation(0);
  ASSERT_EQ(interpolated.size(), uniformSize);
  aurora::Mat3x4<float> slot0Midpoint{};
  aurora::Mat3x4<float> slot1Midpoint{};
  std::memcpy(static_cast<void*>(&slot0Midpoint), interpolated.data() + positionOffset,
              sizeof(slot0Midpoint));
  std::memcpy(static_cast<void*>(&slot1Midpoint),
              interpolated.data() + positionOffset + sizeof(slot0Midpoint),
              sizeof(slot1Midpoint));
  EXPECT_FLOAT_EQ(slot0Midpoint.m0.w(), 45.0f);
  EXPECT_FLOAT_EQ(slot1Midpoint.m0.w(), 55.0f);

  // A changed vertex-to-slot topology is not recoverable from matrix values, so it must not fall
  // into the coarse material-only bucket even when the used mask is unchanged.
  auto changedTopology = identity;
  ++changedTopology.combined;
  ++changedTopology.matrixTopology;
  aurora::gfx::testing::reset_uniform_allocations();
  std::array<uint8_t, uniformSize> changedSource{};
  aurora::gx::begin_frame_interpolation();
  const auto changedRanges = recordFrame(changedTopology, 91.0f, 9.0f, changedSource);
  ASSERT_NE(changedRanges[0].size, 0u);
  aurora::gx::finalize_frame_interpolation();

  // The palette fallback stages the draw before matching. A topology mismatch must leave that
  // staged copy at the current-frame values instead of borrowing the previous palette.
  const auto& changed = aurora::gfx::testing::uniform_allocation(0);
  ASSERT_EQ(changed.size(), uniformSize);
  std::memcpy(static_cast<void*>(&slot0Midpoint), changed.data() + positionOffset,
              sizeof(slot0Midpoint));
  std::memcpy(static_cast<void*>(&slot1Midpoint),
              changed.data() + positionOffset + sizeof(slot0Midpoint), sizeof(slot1Midpoint));
  EXPECT_FLOAT_EQ(slot0Midpoint.m0.w(), 91.0f);
  EXPECT_FLOAT_EQ(slot1Midpoint.m0.w(), 9.0f);

  aurora::gx::set_frame_interpolation_fps(0);
  aurora::gx::begin_frame_interpolation();
  aurora::gfx::testing::reset_uniform_allocations();
}

TEST(FrameInterpolationContract, RepeatedDrawsMatchByTransformInsteadOfDrawOrder) {
  const aurora::Mat3x4<float> left{
      {1.0f, 0.0f, 0.0f, -500.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 1000.0f},
  };
  const aurora::Mat3x4<float> right{
      {1.0f, 0.0f, 0.0f, 500.0f},
      {0.0f, 1.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f, 1000.0f},
  };
  const aurora::Mat3x4<float> currentRight{
      {0.9998f, 0.0f, 0.02f, 496.0f},
      {0.0f, 1.0f, 0.0f, 1.0f},
      {-0.02f, 0.0f, 0.9998f, 997.0f},
  };

  // A transparent sorter may submit right then left this frame after left then right last frame.
  // The signatures are identical, so spatial continuity has to pick the historical transform.
  EXPECT_LT(aurora::gx::transform_match_distance_squared(right, currentRight),
            aurora::gx::transform_match_distance_squared(left, currentRight));
}

TEST(VertexColorContract, MissingAndSparseColorsFollowGxRasterDefaults) {
  aurora::gx::ShaderConfig config{};

  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 0), -1);
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 1), -1);

  config.attrs[GX_VA_CLR1].attrType = GX_DIRECT;
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 0), GX_VA_CLR1);
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 1), -1);

  config.attrs[GX_VA_CLR0].attrType = GX_DIRECT;
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 0), GX_VA_CLR0);
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 1), GX_VA_CLR1);

  config.attrs[GX_VA_CLR1].attrType = GX_NONE;
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 0), GX_VA_CLR0);
  EXPECT_EQ(aurora::gx::shader_vertex_color_attr(config, 1), -1);
}

TEST(TevTexcoordStateContract, DirectStageFeedsFollowingAddPrevInFixedPoint) {
  aurora::gx::ShaderConfig config{};
  config.numTexGens = 2;
  config.numIndStages = 1;
  config.tevStageCount = 2;
  config.indStages[0].texCoordId = GX_TEXCOORD0;
  config.indStages[0].texMapId = GX_TEXMAP0;
  config.indStages[0].scaleS = GX_ITS_1;
  config.indStages[0].scaleT = GX_ITS_1;

  config.tevStages[0].texCoordId = GX_TEXCOORD1;
  config.tevStages[0].texMapId = GX_TEXMAP_NULL;
  config.tevStages[0].indTexMtxId = GX_ITM_OFF;
  config.tevStages[1].indTexMtxId = GX_ITM_1;
  config.tevStages[1].indTexAddPrev = true;
  config.tevStages[1].indTexWrapS = GX_ITW_0;
  config.tevStages[1].indTexWrapT = GX_ITW_0;
  config.tevStages[1].texCoordId = GX_TEXCOORD_NULL;
  config.tevStages[1].texMapId = GX_TEXMAP1;
  config.tevStages[1].colorPass.d = GX_CC_TEXC;

  // Stage 0 is direct and stage 1 consumes the coordinate it establishes. Disabling stage 1's
  // texture map does not stop that update, so its GX_TEXCOORD_NULL resolves to coord 0.
  const auto directDependency = aurora::gx::tev_stage_texture_dependency(config, 0);
  const auto indirectDependency = aurora::gx::tev_stage_texture_dependency(config, 1);
  EXPECT_EQ(directDependency.texCoordId, 1);
  EXPECT_EQ(directDependency.texMapId, -1);
  EXPECT_TRUE(directDependency.needsFixedTexcoordState);
  EXPECT_FALSE(directDependency.canSampleTexture);
  EXPECT_EQ(indirectDependency.texCoordId, 0);
  EXPECT_EQ(indirectDependency.texMapId, 1);
  EXPECT_TRUE(indirectDependency.needsFixedTexcoordState);
  EXPECT_TRUE(indirectDependency.combinerUsesTexture);
  EXPECT_TRUE(indirectDependency.canSampleTexture);
  EXPECT_TRUE(aurora::gx::shader_uses_fixed_texcoord_state(config));

  // The production ShaderInfo implementation, not the old empty stub: both effective coords, the
  // regular texture and the indirect lookup have to appear in the binding contract.
  const auto info = aurora::gx::build_shader_info(config);
  EXPECT_TRUE(info.sampledTexCoords.test(0));
  EXPECT_TRUE(info.sampledTexCoords.test(1));
  EXPECT_TRUE(info.sampledTextures.test(0));
  EXPECT_TRUE(info.sampledTextures.test(1));
  EXPECT_TRUE(info.usedIndStages.test(0));
  EXPECT_TRUE(info.usedIndTexMtxs.test(GX_ITM_1 - GX_ITM_0));
  EXPECT_EQ(aurora::gx::tev_indirect_wrap_mask(GX_ITW_256), 0x7fff);
  EXPECT_EQ(aurora::gx::tev_indirect_wrap_mask(static_cast<GXIndTexWrap>(GX_MAX_ITWRAP)), 0);
  EXPECT_EQ(aurora::gx::indirect_matrix_mantissa(0.5f), 512);
  EXPECT_EQ(aurora::gx::indirect_matrix_mantissa(-0.5f), -512);
  EXPECT_EQ(aurora::gx::indirect_matrix_shift(3), -3);
  EXPECT_EQ(aurora::gx::indirect_matrix_shift(-5), 5);
  EXPECT_EQ(aurora::gx::tev_s24_wrap(0x007fffff), 0x007fffff);
  EXPECT_EQ(aurora::gx::tev_s24_wrap(0x00800000), -0x00800000);
  EXPECT_EQ(aurora::gx::tev_s24_wrap(-0x00800000), -0x00800000);
  EXPECT_EQ(aurora::gx::tev_s24_wrap(-0x00800001), 0x007fffff);

  config.tevStages[1].texMapId = GX_TEXMAP_NULL;
  EXPECT_FALSE(aurora::gx::tev_stage_texture_dependency(config, 1).canSampleTexture);
  config.tevStages[1].texMapId = GX_TEXMAP1;

  // No regular texture lookup occurs with zero texgens, but an enabled
  // indirect stage still samples its texture at the literal zero coordinate.
  config.numTexGens = 0;
  EXPECT_EQ(aurora::gx::tev_stage_texture_dependency(config, 1).texCoordId, -1);
  EXPECT_FALSE(aurora::gx::tev_stage_texture_dependency(config, 1).canSampleTexture);
  const auto zeroTexgenInfo = aurora::gx::build_shader_info(config);
  EXPECT_FALSE(zeroTexgenInfo.sampledTexCoords.any());
  EXPECT_TRUE(zeroTexgenInfo.sampledTextures.test(0));
  EXPECT_FALSE(zeroTexgenInfo.sampledTextures.test(1));
  // WGSL must likewise avoid producing a regular-texture UV expression for
  // stage 1: tex1_size_bias is intentionally absent from this uniform layout.
  const auto zeroTexgenDependency = aurora::gx::tev_stage_texture_dependency(config, 1);
  EXPECT_FALSE(aurora::gx::tev_texture_sample_enabled(zeroTexgenDependency,
                                                      zeroTexgenDependency.combinerUsesTexture));

  // A standalone direct stage still uses the normalized fast path.
  config.numTexGens = 2;
  config.tevStageCount = 1;
  config.tevStages[1] = {};
  config.tevStages[0].texMapId = GX_TEXMAP1;
  config.tevStages[0].colorPass.d = GX_CC_TEXC;
  const auto directSampleDependency = aurora::gx::tev_stage_texture_dependency(config, 0);
  EXPECT_FALSE(directSampleDependency.needsFixedTexcoordState);
  EXPECT_TRUE(directSampleDependency.combinerUsesTexture);
  EXPECT_TRUE(directSampleDependency.canSampleTexture);
  const auto directInfo = aurora::gx::build_shader_info(config);
  EXPECT_TRUE(directInfo.sampledTexCoords.test(1));
  EXPECT_TRUE(directInfo.sampledTextures.test(1));

  // Z-texture remains enabled even without a regular source stage; the shader
  // applies its bias/op to a zero raw texture value in that case.
  config.tevStages[0].texMapId = GX_TEXMAP_NULL;
  config.tevStages[0].colorPass.d = GX_CC_ZERO;
  config.zTexture = static_cast<u32>(GX_ZT_REPLACE) << 26;
  EXPECT_TRUE(aurora::gx::tev_z_texture_enabled(config));
  EXPECT_EQ(aurora::gx::tev_z_texture_stage(config), -1);

}

TEST(TevRegisterLivenessContract, RgbWriteDoesNotHideSameStageOldAlphaRead) {
  aurora::gx::ShaderConfig config{};
  config.tevStageCount = 1;
  config.tevStages[0].colorOp.outReg = GX_TEVREG0;
  config.tevStages[0].alphaPass.d = GX_CA_A0;

  const auto info = aurora::gx::build_shader_info(config);
  EXPECT_TRUE(info.writesTevRegRgb.test(GX_TEVREG0));
  EXPECT_FALSE(info.writesTevRegAlpha.test(GX_TEVREG0));
  EXPECT_FALSE(info.loadsTevRegRgb.test(GX_TEVREG0));
  EXPECT_TRUE(info.loadsTevRegAlpha.test(GX_TEVREG0));
}

TEST(TevRegisterLivenessContract, TracksOppositeHalvesIndependentlyAcrossStages) {
  aurora::gx::ShaderConfig config{};
  config.tevStageCount = 2;
  config.tevStages[0].colorOp.outReg = GX_TEVREG0;
  config.tevStages[0].alphaOp.outReg = GX_TEVREG1;
  config.tevStages[1].colorPass.a = GX_CC_C1;
  config.tevStages[1].alphaPass.a = GX_CA_A0;

  const auto info = aurora::gx::build_shader_info(config);
  EXPECT_TRUE(info.loadsTevRegAlpha.test(GX_TEVREG0));
  EXPECT_FALSE(info.loadsTevRegRgb.test(GX_TEVREG0));
  EXPECT_TRUE(info.loadsTevRegRgb.test(GX_TEVREG1));
  EXPECT_FALSE(info.loadsTevRegAlpha.test(GX_TEVREG1));
}

TEST(TevRegisterLivenessContract, PacksOneUniformWhenBothHalvesNeedInitialValue) {
  aurora::gx::ShaderConfig baseline{};
  baseline.tevStageCount = 1;

  auto config = baseline;
  config.tevStages[0].colorPass.a = GX_CC_C0;
  config.tevStages[0].alphaPass.a = GX_CA_A0;

  const auto baselineInfo = aurora::gx::build_shader_info(baseline);
  const auto info = aurora::gx::build_shader_info(config);
  EXPECT_TRUE(info.loadsTevRegRgb.test(GX_TEVREG0));
  EXPECT_TRUE(info.loadsTevRegAlpha.test(GX_TEVREG0));
  EXPECT_EQ((info.loadsTevRegRgb | info.loadsTevRegAlpha).count(), 1u);
  // Both halves share one Vec4, and the additional value fits inside the existing aligned range.
  EXPECT_EQ(info.uniformSize, baselineInfo.uniformSize);
}

// BP registers (direct FIFO writes, no dirty state flush needed)

// --- GXSetBlendMode (BP 0x41) ---

TEST_F(GXFifoTest, BlendMode_Blend_SrcAlpha) {
  GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
  auto bytes = capture_fifo();

  // Validate encoding: BP opcode 0x61, register ID 0x41
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x41);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.blendMode, GX_BM_BLEND);
  EXPECT_EQ(g_gxState.blendFacSrc, GX_BL_SRCALPHA);
  EXPECT_EQ(g_gxState.blendFacDst, GX_BL_INVSRCALPHA);
}

TEST_F(GXFifoTest, BlendMode_None) {
  GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_CLEAR);
  auto bytes = capture_fifo();

  reset_gx_state();
  // Pre-set to something else to prove the decode works
  g_gxState.blendMode = GX_BM_BLEND;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.blendMode, GX_BM_NONE);
}

TEST_F(GXFifoTest, BlendMode_Subtract) {
  GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.blendMode, GX_BM_SUBTRACT);
}

TEST_F(GXFifoTest, BlendMode_Logic) {
  GXSetBlendMode(GX_BM_LOGIC, GX_BL_ONE, GX_BL_ZERO, GX_LO_XOR);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.blendMode, GX_BM_LOGIC);
  EXPECT_EQ(g_gxState.blendOp, GX_LO_XOR);
}

TEST_F(GXFifoTest, BpMask_AppliesOnlyToNextWrite) {
  std::vector<u8> bytes;
  auto mask = bp_cmd(0xFE, 1u << 19);
  auto genMode = bp_cmd(0x00, 1u << 19);
  auto cullAndInd = bp_cmd(0x00, (2u << 14) | (3u << 16));
  bytes.insert(bytes.end(), mask.begin(), mask.end());
  bytes.insert(bytes.end(), genMode.begin(), genMode.end());
  bytes.insert(bytes.end(), cullAndInd.begin(), cullAndInd.end());

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.bpRegCache[0x00] & (1u << 19), 0u);
  EXPECT_EQ(g_gxState.cullMode, GX_CULL_FRONT);
  EXPECT_EQ(g_gxState.numIndStages, 3u);
}

TEST_F(GXFifoTest, IndirectTextureMask_DecodesWithoutChangingBpWriteMask) {
  auto bytes = bp_cmd(0x0F, 0x5A);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.indTexMask, 0x5Au);
  EXPECT_EQ(g_gxState.bpRegCache[0x0F], 0x0F00005Au);
  EXPECT_EQ(g_gxState.bpRegCache[0xFE], 0x00FFFFFFu);
}

// --- GXSetColorUpdate / GXSetAlphaUpdate (BP 0x41 cmode0) ---

TEST_F(GXFifoTest, ColorUpdate_Disabled) {
  GXSetColorUpdate(GX_FALSE);
  auto bytes = capture_fifo();

  reset_gx_state();
  g_gxState.colorUpdate = true;
  decode_fifo(bytes);

  EXPECT_FALSE(g_gxState.colorUpdate);
}

TEST_F(GXFifoTest, AlphaUpdate_Disabled) {
  GXSetAlphaUpdate(false);
  auto bytes = capture_fifo();

  reset_gx_state();
  g_gxState.alphaUpdate = true;
  decode_fifo(bytes);

  EXPECT_FALSE(g_gxState.alphaUpdate);
}

// --- GXSetZMode (BP 0x40) ---

TEST_F(GXFifoTest, ZMode_LessNoUpdate) {
  GXSetZMode(true, GX_LESS, false);
  auto bytes = capture_fifo();

  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x40);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_TRUE(g_gxState.depthCompare);
  EXPECT_EQ(g_gxState.depthFunc, GX_LESS);
  EXPECT_FALSE(g_gxState.depthUpdate);
}

TEST_F(GXFifoTest, ZMode_AlwaysUpdate) {
  GXSetZMode(true, GX_ALWAYS, true);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_TRUE(g_gxState.depthCompare);
  EXPECT_EQ(g_gxState.depthFunc, GX_ALWAYS);
  EXPECT_TRUE(g_gxState.depthUpdate);
}

TEST_F(GXFifoTest, ZMode_Disabled) {
  GXSetZMode(false, GX_NEVER, false);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FALSE(g_gxState.depthCompare);
  EXPECT_EQ(g_gxState.depthFunc, GX_NEVER);
  EXPECT_FALSE(g_gxState.depthUpdate);
}

TEST_F(GXFifoTest, ZTexture_ReplaceZ24X8) {
  GXSetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0x123456);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_bp_write(bytes, 0xF4));
  EXPECT_TRUE(has_bp_write(bytes, 0xF5));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.zTextureBias, 0x123456u);
  EXPECT_EQ(g_gxState.zTextureFmt, 2u);
  EXPECT_EQ(g_gxState.zTextureOp, GX_ZT_REPLACE);
}

// --- GXSetAlphaCompare (BP 0xF3) ---

TEST_F(GXFifoTest, AlphaCompare_GreaterThan128) {
  GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
  auto bytes = capture_fifo();

  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xF3);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.alphaCompare.comp0, GX_GREATER);
  EXPECT_EQ(g_gxState.alphaCompare.ref0, 128u);
  EXPECT_EQ(g_gxState.alphaCompare.op, GX_AOP_AND);
  EXPECT_EQ(g_gxState.alphaCompare.comp1, GX_ALWAYS);
  EXPECT_EQ(g_gxState.alphaCompare.ref1, 0u);
}

TEST_F(GXFifoTest, AlphaCompare_OrGequal) {
  GXSetAlphaCompare(GX_GEQUAL, 64, GX_AOP_OR, GX_LEQUAL, 200);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.alphaCompare.comp0, GX_GEQUAL);
  EXPECT_EQ(g_gxState.alphaCompare.ref0, 64u);
  EXPECT_EQ(g_gxState.alphaCompare.op, GX_AOP_OR);
  EXPECT_EQ(g_gxState.alphaCompare.comp1, GX_LEQUAL);
  EXPECT_EQ(g_gxState.alphaCompare.ref1, 200u);
}

// --- GXSetDstAlpha (BP 0x42) ---

TEST_F(GXFifoTest, DstAlpha_Enabled) {
  GXSetDstAlpha(true, 0x80);
  auto bytes = capture_fifo();

  reset_gx_state();
  g_gxState.dstAlpha = UINT32_MAX;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.dstAlpha, 0x80u);
}

TEST_F(GXFifoTest, DstAlpha_Disabled) {
  GXSetDstAlpha(false, 0);
  auto bytes = capture_fifo();

  reset_gx_state();
  g_gxState.dstAlpha = 0x80;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.dstAlpha, UINT32_MAX);
}

// --- GXSetPixelFmt (BP 0x43, 0x42 + genMode flush) ---

TEST_F(GXFifoTest, PixelFmt_Rgb565Z16_Decode) {
  GXSetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_FAR);
  auto bytes = flush_and_capture();

  EXPECT_TRUE(has_bp_write(bytes, 0x43));
  EXPECT_TRUE(has_bp_write(bytes, 0x00));

  reset_gx_state();
  g_gxState.pixelFmt = GX_PF_RGB8_Z24;
  g_gxState.zFmt = GX_ZC_LINEAR;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.pixelFmt, GX_PF_RGB565_Z16);
  EXPECT_EQ(g_gxState.zFmt, GX_ZC_FAR);
  EXPECT_TRUE(g_gxState.zCompLocBeforeTex);
}

TEST_F(GXFifoTest, PixelFmt_U8_Decode) {
  GXSetPixelFmt(GX_PF_U8, GX_ZC_MID);
  auto bytes = flush_and_capture();

  EXPECT_TRUE(has_bp_write(bytes, 0x43));
  EXPECT_TRUE(has_bp_write(bytes, 0x42));
  EXPECT_TRUE(has_bp_write(bytes, 0x00));

  reset_gx_state();
  g_gxState.pixelFmt = GX_PF_RGB8_Z24;
  g_gxState.zFmt = GX_ZC_LINEAR;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.pixelFmt, GX_PF_U8);
  EXPECT_EQ(g_gxState.zFmt, GX_ZC_MID);
  EXPECT_EQ(g_gxState.dstAlpha, UINT32_MAX);
  EXPECT_TRUE(g_gxState.zCompLocBeforeTex);
}

// TEV registers (direct FIFO writes)

// --- GXSetTevColorIn / GXSetTevAlphaIn ---

TEST_F(GXFifoTest, TevColorIn_Stage0) {
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
  auto bytes = capture_fifo();

  // BP opcode 0x61, register 0xC0 (stage 0 color)
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xC0);

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.colorPass.a, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.b, GX_CC_TEXC);
  EXPECT_EQ(s.colorPass.c, GX_CC_RASC);
  EXPECT_EQ(s.colorPass.d, GX_CC_ZERO);
}

TEST_F(GXFifoTest, TevAlphaIn_Stage0) {
  GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
  auto bytes = capture_fifo();

  // BP opcode 0x61, register 0xC1 (stage 0 alpha)
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xC1);

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_TEXA);
  EXPECT_EQ(s.alphaPass.c, GX_CA_RASA);
  EXPECT_EQ(s.alphaPass.d, GX_CA_ZERO);
}

TEST_F(GXFifoTest, TevAlphaIn_Stage5) {
  GXSetTevAlphaIn(GX_TEVSTAGE5, GX_CA_APREV, GX_CA_A0, GX_CA_KONST, GX_CA_ZERO);
  auto bytes = capture_fifo();

  // Stage 5 alpha register = 0xC1 + 5*2 = 0xCB
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xCB);

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[5];
  EXPECT_EQ(s.alphaPass.a, GX_CA_APREV);
  EXPECT_EQ(s.alphaPass.b, GX_CA_A0);
  EXPECT_EQ(s.alphaPass.c, GX_CA_KONST);
  EXPECT_EQ(s.alphaPass.d, GX_CA_ZERO);
}

TEST_F(GXFifoTest, TevColorIn_Stage7) {
  GXSetTevColorIn(GX_TEVSTAGE7, GX_CC_C0, GX_CC_A0, GX_CC_KONST, GX_CC_CPREV);
  auto bytes = capture_fifo();

  // Stage 7 color register = 0xC0 + 7*2 = 0xCE
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xCE);

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[7];
  EXPECT_EQ(s.colorPass.a, GX_CC_C0);
  EXPECT_EQ(s.colorPass.b, GX_CC_A0);
  EXPECT_EQ(s.colorPass.c, GX_CC_KONST);
  EXPECT_EQ(s.colorPass.d, GX_CC_CPREV);
}

// --- GXSetTevOp (convenience wrapper over ColorIn/AlphaIn/ColorOp/AlphaOp) ---
// GXSetTevOp emits 4 BP writes: tevc (colorIn+colorOp) and teva (alphaIn+alphaOp).

TEST_F(GXFifoTest, TevOp_Modulate_Stage0) {
  GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // Modulate: color = ZERO, TEXC, RASC, ZERO (stage 0 uses RASC/RASA)
  EXPECT_EQ(s.colorPass.a, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.b, GX_CC_TEXC);
  EXPECT_EQ(s.colorPass.c, GX_CC_RASC);
  EXPECT_EQ(s.colorPass.d, GX_CC_ZERO);
  // Modulate: alpha = ZERO, TEXA, RASA, ZERO
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_TEXA);
  EXPECT_EQ(s.alphaPass.c, GX_CA_RASA);
  EXPECT_EQ(s.alphaPass.d, GX_CA_ZERO);
  // Op = ADD, bias = ZERO, scale = 1, clamp = true, outReg = TEVPREV
  EXPECT_EQ(s.colorOp.op, GX_TEV_ADD);
  EXPECT_EQ(s.colorOp.bias, GX_TB_ZERO);
  EXPECT_EQ(s.colorOp.scale, GX_CS_SCALE_1);
  EXPECT_TRUE(s.colorOp.clamp);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVPREV);
  EXPECT_EQ(s.alphaOp.op, GX_TEV_ADD);
  EXPECT_EQ(s.alphaOp.bias, GX_TB_ZERO);
  EXPECT_EQ(s.alphaOp.scale, GX_CS_SCALE_1);
  EXPECT_TRUE(s.alphaOp.clamp);
  EXPECT_EQ(s.alphaOp.outReg, GX_TEVPREV);
}

TEST_F(GXFifoTest, TevOp_Modulate_Stage1) {
  // Non-stage-0 uses CPREV/APREV instead of RASC/RASA
  GXSetTevOp(GX_TEVSTAGE1, GX_MODULATE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[1];
  EXPECT_EQ(s.colorPass.a, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.b, GX_CC_TEXC);
  EXPECT_EQ(s.colorPass.c, GX_CC_CPREV);
  EXPECT_EQ(s.colorPass.d, GX_CC_ZERO);
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_TEXA);
  EXPECT_EQ(s.alphaPass.c, GX_CA_APREV);
  EXPECT_EQ(s.alphaPass.d, GX_CA_ZERO);
}

TEST_F(GXFifoTest, TevOp_Replace) {
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // Replace: color = ZERO, ZERO, ZERO, TEXC
  EXPECT_EQ(s.colorPass.a, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.b, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.c, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.d, GX_CC_TEXC);
  // Replace: alpha = ZERO, ZERO, ZERO, TEXA
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.c, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.d, GX_CA_TEXA);
}

TEST_F(GXFifoTest, TevOp_Decal) {
  GXSetTevOp(GX_TEVSTAGE0, GX_DECAL);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // Decal: color = RASC, TEXC, TEXA, ZERO
  EXPECT_EQ(s.colorPass.a, GX_CC_RASC);
  EXPECT_EQ(s.colorPass.b, GX_CC_TEXC);
  EXPECT_EQ(s.colorPass.c, GX_CC_TEXA);
  EXPECT_EQ(s.colorPass.d, GX_CC_ZERO);
  // Decal: alpha = ZERO, ZERO, ZERO, RASA
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.c, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.d, GX_CA_RASA);
}

TEST_F(GXFifoTest, TevOp_Blend) {
  GXSetTevOp(GX_TEVSTAGE0, GX_BLEND);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // Blend: color = RASC, ONE, TEXC, ZERO
  EXPECT_EQ(s.colorPass.a, GX_CC_RASC);
  EXPECT_EQ(s.colorPass.b, GX_CC_ONE);
  EXPECT_EQ(s.colorPass.c, GX_CC_TEXC);
  EXPECT_EQ(s.colorPass.d, GX_CC_ZERO);
  // Blend: alpha = ZERO, TEXA, RASA, ZERO
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_TEXA);
  EXPECT_EQ(s.alphaPass.c, GX_CA_RASA);
  EXPECT_EQ(s.alphaPass.d, GX_CA_ZERO);
}

TEST_F(GXFifoTest, TevOp_PassClr) {
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // PassClr: color = ZERO, ZERO, ZERO, RASC
  EXPECT_EQ(s.colorPass.a, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.b, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.c, GX_CC_ZERO);
  EXPECT_EQ(s.colorPass.d, GX_CC_RASC);
  // PassClr: alpha = ZERO, ZERO, ZERO, RASA
  EXPECT_EQ(s.alphaPass.a, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.b, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.c, GX_CA_ZERO);
  EXPECT_EQ(s.alphaPass.d, GX_CA_RASA);
}

// --- GXSetTevColorOp / GXSetTevAlphaOp ---

TEST_F(GXFifoTest, TevColorOp_Sub_Scale2_Reg1) {
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ADDHALF, GX_CS_SCALE_2, GX_TRUE, GX_TEVREG1);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.colorOp.op, GX_TEV_SUB);
  EXPECT_EQ(s.colorOp.bias, GX_TB_ADDHALF);
  EXPECT_EQ(s.colorOp.scale, GX_CS_SCALE_2);
  EXPECT_TRUE(s.colorOp.clamp);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVREG1);
}

TEST_F(GXFifoTest, TevAlphaOp_Add_NoClamp_Reg2) {
  GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_DIVIDE_2, GX_FALSE, GX_TEVREG2);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.alphaOp.op, GX_TEV_ADD);
  EXPECT_EQ(s.alphaOp.bias, GX_TB_SUBHALF);
  EXPECT_EQ(s.alphaOp.scale, GX_CS_DIVIDE_2);
  EXPECT_FALSE(s.alphaOp.clamp);
  EXPECT_EQ(s.alphaOp.outReg, GX_TEVREG2);
}

TEST_F(GXFifoTest, TevColorOp_CompareR8GT) {
  // Compare ops (op > 1) use a different encoding: bias=3, scale encodes compare mode
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_R8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.colorOp.op, GX_TEV_COMP_R8_GT);
  // Decoder normalizes compare mode: bias=ZERO, scale=SCALE_1
  EXPECT_EQ(s.colorOp.bias, GX_TB_ZERO);
  EXPECT_EQ(s.colorOp.scale, GX_CS_SCALE_1);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVPREV);
}

TEST_F(GXFifoTest, TevColorOp_CompareGR16EQ) {
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_GR16_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.colorOp.op, GX_TEV_COMP_GR16_EQ);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVREG0);
}

TEST_F(GXFifoTest, TevAlphaOp_CompareRGB8GT) {
  GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  // GX_TEV_COMP_RGB8_GT is the same enum value for alpha as GX_TEV_COMP_A8_GT
  EXPECT_EQ(s.alphaOp.op, GX_TEV_COMP_RGB8_GT);
  EXPECT_EQ(s.alphaOp.bias, GX_TB_ZERO);
  EXPECT_EQ(s.alphaOp.scale, GX_CS_SCALE_1);
}

TEST_F(GXFifoTest, TevColorOp_CompareBGR24GT) {
  GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_COMP_BGR24_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[2];
  EXPECT_EQ(s.colorOp.op, GX_TEV_COMP_BGR24_GT);
  EXPECT_EQ(s.colorOp.bias, GX_TB_ZERO);
  EXPECT_EQ(s.colorOp.scale, GX_CS_SCALE_1);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVREG1);
}

TEST_F(GXFifoTest, TevColorOp_CompareRGB8EQ) {
  GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_COMP_RGB8_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.colorOp.op, GX_TEV_COMP_RGB8_EQ);
  EXPECT_EQ(s.colorOp.outReg, GX_TEVPREV);
}

TEST_F(GXFifoTest, TevAlphaOp_CompareA8EQ) {
  GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_COMP_RGB8_EQ, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG2);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[1];
  // For alpha, GX_TEV_COMP_RGB8_EQ maps to A8_EQ
  EXPECT_EQ(s.alphaOp.op, GX_TEV_COMP_RGB8_EQ);
  EXPECT_EQ(s.alphaOp.outReg, GX_TEVREG2);
}

// --- GXSetTevColorS10 ---

TEST_F(GXFifoTest, TevColorS10_Positive) {
  GXColorS10 col = {511, 256, 100, 0};
  GXSetTevColorS10(GX_TEVREG0, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // S10 values are encoded as 11-bit signed and decoded to float/255
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][0], 511.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][1], 256.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][2], 100.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][3], 0.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevColorS10_Negative) {
  GXColorS10 col = {-128, -1, 0, 255};
  GXSetTevColorS10(GX_TEVPREV, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][0], -128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][1], -1.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][2], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][3], 255.f / 255.f, 1.f / 255.f);
}

// --- GXSetTevKColorSel / GXSetTevKAlphaSel ---

TEST_F(GXFifoTest, TevKColorSel_Stage0_K0) {
  GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[0].kcSel, GX_TEV_KCSEL_K0);
}

TEST_F(GXFifoTest, TevKColorSel_Stage1_K2_R) {
  GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K2_R);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[1].kcSel, GX_TEV_KCSEL_K2_R);
}

TEST_F(GXFifoTest, TevKAlphaSel_Stage0_K1_A) {
  GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K1_A);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[0].kaSel, GX_TEV_KASEL_K1_A);
}

TEST_F(GXFifoTest, TevKAlphaSel_Stage3_K3_B) {
  GXSetTevKAlphaSel(GX_TEVSTAGE3, GX_TEV_KASEL_K3_B);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[3].kaSel, GX_TEV_KASEL_K3_B);
}

TEST_F(GXFifoTest, TevKColorSel_DoesNotCorruptSwapTable) {
  // Regression: tevKsel shadow registers share bits with swap table entries.
  // Setting K color selection must not zero out the swap table bits.
  GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Swap table 0 should remain identity (initialized in GXInit)
  EXPECT_EQ(g_gxState.tevSwapTable[0].red, GX_CH_RED);
  EXPECT_EQ(g_gxState.tevSwapTable[0].green, GX_CH_GREEN);
  EXPECT_EQ(g_gxState.tevSwapTable[0].blue, GX_CH_BLUE);
  EXPECT_EQ(g_gxState.tevSwapTable[0].alpha, GX_CH_ALPHA);
  // K color selection should still be set
  EXPECT_EQ(g_gxState.tevStages[0].kcSel, GX_TEV_KCSEL_K0);
}

// --- GXSetTevSwapMode ---

TEST_F(GXFifoTest, TevSwapMode_Stage0) {
  GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP1, GX_TEV_SWAP2);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[0].tevSwapRas, GX_TEV_SWAP1);
  EXPECT_EQ(g_gxState.tevStages[0].tevSwapTex, GX_TEV_SWAP2);
}

TEST_F(GXFifoTest, TevSwapMode_Stage3) {
  GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP3, GX_TEV_SWAP0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevStages[3].tevSwapRas, GX_TEV_SWAP3);
  EXPECT_EQ(g_gxState.tevStages[3].tevSwapTex, GX_TEV_SWAP0);
}

// --- GXSetTevSwapModeTable ---

TEST_F(GXFifoTest, TevSwapModeTable_Swap1_AllRed) {
  GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP1].red, GX_CH_RED);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP1].green, GX_CH_RED);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP1].blue, GX_CH_RED);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP1].alpha, GX_CH_ALPHA);
}

TEST_F(GXFifoTest, TevSwapModeTable_Swap2_Swizzle) {
  GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_BLUE, GX_CH_GREEN, GX_CH_RED, GX_CH_ALPHA);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP2].red, GX_CH_BLUE);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP2].green, GX_CH_GREEN);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP2].blue, GX_CH_RED);
  EXPECT_EQ(g_gxState.tevSwapTable[GX_TEV_SWAP2].alpha, GX_CH_ALPHA);
}

// --- GXSetTevOrder (BP 0x28-0x2F) ---

TEST_F(GXFifoTest, TevOrder_Stage0) {
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.texMapId, GX_TEXMAP0);
  EXPECT_EQ(s.texCoordId, GX_TEXCOORD0);
  EXPECT_EQ(s.channelId, GX_COLOR0A0);
}

TEST_F(GXFifoTest, TevOrder_Stage1_OddStage) {
  // Odd stages use different bit positions within the tref register
  GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD2, GX_TEXMAP3, GX_COLOR1A1);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[1];
  EXPECT_EQ(s.texMapId, GX_TEXMAP3);
  EXPECT_EQ(s.texCoordId, GX_TEXCOORD2);
  EXPECT_EQ(s.channelId, GX_COLOR1A1);
}

TEST_F(GXFifoTest, TevOrder_Stage0_TexNull) {
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& s = g_gxState.tevStages[0];
  EXPECT_EQ(s.texMapId, GX_TEXMAP_NULL);
  EXPECT_EQ(s.channelId, GX_COLOR0A0);
}

// --- GXSetTevKColor (BP 0xE0-0xE7, K color flag) ---

TEST_F(GXFifoTest, TevKColor_K0) {
  GXColor kc = {255, 128, 64, 32};
  GXSetTevKColor(GX_KCOLOR0, kc);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // K colors are stored as float (0-1 range), 8-bit precision
  EXPECT_NEAR(g_gxState.kcolors[0][0], 255.f / 255.f, 1.f / 255.f); // R
  EXPECT_NEAR(g_gxState.kcolors[0][1], 128.f / 255.f, 1.f / 255.f); // G
  EXPECT_NEAR(g_gxState.kcolors[0][2], 64.f / 255.f, 1.f / 255.f);  // B
  EXPECT_NEAR(g_gxState.kcolors[0][3], 32.f / 255.f, 1.f / 255.f);  // A
}

TEST_F(GXFifoTest, TevKColor_K1) {
  GXColor kc = {0, 255, 0, 128};
  GXSetTevKColor(GX_KCOLOR1, kc);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.kcolors[1][0], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[1][1], 255.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[1][2], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[1][3], 128.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevKColor_K2) {
  GXColor kc = {10, 20, 30, 40};
  GXSetTevKColor(GX_KCOLOR2, kc);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.kcolors[2][0], 10.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[2][1], 20.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[2][2], 30.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[2][3], 40.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevKColor_K3) {
  GXColor kc = {200, 150, 100, 50};
  GXSetTevKColor(GX_KCOLOR3, kc);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.kcolors[3][0], 200.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[3][1], 150.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[3][2], 100.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.kcolors[3][3], 50.f / 255.f, 1.f / 255.f);
}

// GXSetTevColor (BP 0xE0-0xE7): the side channel stores float while the FIFO encodes 11-bit
// signed, so the decoded value comes back with reduced precision.

TEST_F(GXFifoTest, TevColor_Reg0) {
  GXColor col = {200, 100, 50, 255};
  GXSetTevColor(GX_TEVREG0, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // 11-bit signed encoding, so values should round-trip within 8-bit range
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][0], 200.f / 255.f, 1.f / 255.f); // R
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][1], 100.f / 255.f, 1.f / 255.f); // G
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][2], 50.f / 255.f, 1.f / 255.f);  // B
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][3], 255.f / 255.f, 1.f / 255.f); // A
}

TEST_F(GXFifoTest, TevColor_Prev) {
  GXColor col = {128, 64, 32, 16};
  GXSetTevColor(GX_TEVPREV, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][0], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][1], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][2], 32.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVPREV][3], 16.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevColor_Reg1) {
  GXColor col = {0, 128, 255, 192};
  GXSetTevColor(GX_TEVREG1, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][0], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][1], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][2], 255.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][3], 192.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevColor_Reg2) {
  GXColor col = {1, 2, 3, 4};
  GXSetTevColor(GX_TEVREG2, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][0], 1.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][1], 2.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][2], 3.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][3], 4.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevColorS10_Reg1) {
  GXColorS10 col = {300, -50, 0, 255};
  GXSetTevColorS10(GX_TEVREG1, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][0], 300.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][1], -50.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][2], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG1][3], 255.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, TevColorS10_Reg2) {
  GXColorS10 col = {-1024, 1023, 128, -256};
  GXSetTevColorS10(GX_TEVREG2, col);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][0], -1024.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][1], 1023.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][2], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG2][3], -256.f / 255.f, 1.f / 255.f);
}

// CP registers (require __GXSetDirtyState() flush)

// --- GXClearVtxDesc ---

TEST_F(GXFifoTest, ClearVtxDesc_ClearsAll) {
  // Set every attribute to something non-default
  GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX1MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX2MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX3MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX4MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX5MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX6MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX7MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
  GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR1, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
  GXSetVtxDesc(GX_VA_TEX1, GX_INDEX8);
  GXSetVtxDesc(GX_VA_TEX2, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX3, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX4, GX_INDEX16);
  GXSetVtxDesc(GX_VA_TEX5, GX_INDEX8);
  GXSetVtxDesc(GX_VA_TEX6, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX7, GX_DIRECT);
  // Discard the dirty state from above
  aurora::gx::fifo::clear_buffer();

  // Now clear and flush
  GXClearVtxDesc();
  auto bytes = flush_and_capture();

  reset_gx_state();
  // Pre-fill g_gxState with non-zero to prove decode clears them
  for (int i = 0; i < GX_VA_MAX_ATTR; ++i) {
    g_gxState.vtxDesc[i] = GX_INDEX16;
  }
  decode_fifo(bytes);

  // After GXClearVtxDesc: POS = GX_DIRECT, everything else = GX_NONE
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_PNMTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX0MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX1MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX2MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX3MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX4MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX5MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX6MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX7MTXIDX], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_POS], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_NRM], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_CLR0], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_CLR1], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX0], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX1], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX2], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX3], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX4], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX5], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX6], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX7], GX_NONE);
}

// --- GXSetVtxDesc / GXClearVtxDesc ---

TEST_F(GXFifoTest, VtxDesc_PosAndNrm_Direct) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_POS], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_NRM], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_CLR0], GX_NONE);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX0], GX_NONE);
}

TEST_F(GXFifoTest, VtxDesc_Indexed) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
  GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_POS], GX_INDEX16);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_NRM], GX_INDEX16);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_CLR0], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX0], GX_INDEX8);
  EXPECT_EQ(g_gxState.sourceVtxDesc[GX_VA_POS], GX_INDEX16);
  EXPECT_EQ(g_gxState.sourceVtxDesc[GX_VA_NRM], GX_INDEX16);
  EXPECT_EQ(g_gxState.sourceVtxDesc[GX_VA_CLR0], GX_DIRECT);
  EXPECT_EQ(g_gxState.sourceVtxDesc[GX_VA_TEX0], GX_INDEX8);
}

TEST_F(GXFifoTest, SourceVtxDesc_DoesNotChangeEmittedLayout) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);
  GXSetSourceVtxDesc(GX_VA_POS, GX_INDEX16);

  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_POS], GX_DIRECT);
  EXPECT_EQ(g_gxState.sourceVtxDesc[GX_VA_POS], GX_INDEX16);
}

TEST_F(GXFifoTest, VtxDesc_MtxIdx) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_PNMTXIDX], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_TEX0MTXIDX], GX_DIRECT);
  EXPECT_EQ(g_gxState.vtxDesc[GX_VA_POS], GX_DIRECT);
}

TEST_F(GXFifoTest, GetVtxDesc_UsesShadowState) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
  GXSetVtxDesc(GX_VA_NBT, GX_DIRECT);

  GXAttrType posType = GX_NONE;
  GXAttrType nbtType = GX_NONE;
  GXVtxDescList vcd[24]{};
  GXGetVtxDesc(GX_VA_POS, &posType);
  GXGetVtxDesc(GX_VA_NBT, &nbtType);
  GXGetVtxDescv(vcd);

  EXPECT_EQ(posType, GX_INDEX16);
  EXPECT_EQ(nbtType, GX_DIRECT);
  EXPECT_EQ(vcd[GX_VA_POS].attr, GX_VA_POS);
  EXPECT_EQ(vcd[GX_VA_POS].type, GX_INDEX16);
  EXPECT_EQ(vcd[GX_VA_TEX7 + 1].attr, GX_VA_NBT);
  EXPECT_EQ(vcd[GX_VA_TEX7 + 1].type, GX_DIRECT);
  EXPECT_EQ(vcd[GX_VA_TEX7 + 2].attr, GX_VA_NULL);
}

// --- GXSetVtxAttrFmt ---

TEST_F(GXFifoTest, VtxAttrFmt_PosF32) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_POS].cnt, GX_POS_XYZ);
  EXPECT_EQ(vf.attrs[GX_VA_POS].type, GX_F32);
  EXPECT_EQ(vf.attrs[GX_VA_POS].frac, 0);
}

TEST_F(GXFifoTest, VtxAttrFmt_NrmS16) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_NRM].cnt, GX_NRM_XYZ);
  EXPECT_EQ(vf.attrs[GX_VA_NRM].type, GX_S16);
  EXPECT_EQ(vf.attrs[GX_VA_NRM].frac, 14);
}

TEST_F(GXFifoTest, VtxAttrFmt_NrmU8UsesUnsignedScale) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0);
  (void)flush_and_capture();
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_U8, 0);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_NRM].cnt, GX_NRM_XYZ);
  EXPECT_EQ(vf.attrs[GX_VA_NRM].type, GX_U8);
  EXPECT_EQ(vf.attrs[GX_VA_NRM].frac, 7);
}

TEST_F(GXFifoTest, NormalU8DirectPreservesUnsignedRawBytes) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_NRM, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_U8, 0);
  aurora::gx::fifo::clear_buffer();

  GXNormal3u8(0xFF, 0x80, 0x01);
  const auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 3u);
  EXPECT_EQ(bytes[0], 0xFF);
  EXPECT_EQ(bytes[1], 0x80);
  EXPECT_EQ(bytes[2], 0x01);
}

TEST_F(GXFifoTest, VtxAttrFmt_NrmNBT3PreservesIndex3Bit) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_NBT3, GX_S16, 0);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_NRM].cnt, GX_NRM_NBT3);
  EXPECT_EQ(vf.attrs[GX_VA_NRM].type, GX_S16);
}

TEST_F(GXFifoTest, VtxAttrFmt_Tex0_S16_Frac8) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 8);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_TEX0].cnt, GX_TEX_ST);
  EXPECT_EQ(vf.attrs[GX_VA_TEX0].type, GX_S16);
  EXPECT_EQ(vf.attrs[GX_VA_TEX0].frac, 8);
}

TEST_F(GXFifoTest, VtxAttrFmt_Clr0_RGBA8) {
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT0];
  EXPECT_EQ(vf.attrs[GX_VA_CLR0].cnt, GX_CLR_RGBA);
  EXPECT_EQ(vf.attrs[GX_VA_CLR0].type, GX_RGBA8);
}

TEST_F(GXFifoTest, VtxAttrFmt_MultipleTexCoords) {
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_U16, 15);
  GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX2, GX_TEX_ST, GX_S16, 8);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  auto& vf = g_gxState.vtxFmts[GX_VTXFMT1];
  EXPECT_EQ(vf.attrs[GX_VA_TEX0].type, GX_F32);
  EXPECT_EQ(vf.attrs[GX_VA_TEX1].type, GX_U16);
  EXPECT_EQ(vf.attrs[GX_VA_TEX1].frac, 15);
  EXPECT_EQ(vf.attrs[GX_VA_TEX2].type, GX_S16);
  EXPECT_EQ(vf.attrs[GX_VA_TEX2].frac, 8);
}

TEST_F(GXFifoTest, GetVtxAttrFmt_UsesShadowState) {
  GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, GX_NRM_NBT3, GX_S16, 0);
  GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX4, GX_TEX_ST, GX_U16, 11);

  GXCompCnt cnt = GX_POS_XY;
  GXCompType type = GX_U8;
  u8 frac = 0;
  GXVtxAttrFmtList vat[13]{};

  GXGetVtxAttrFmt(GX_VTXFMT2, GX_VA_NRM, &cnt, &type, &frac);
  EXPECT_EQ(cnt, GX_NRM_NBT3);
  EXPECT_EQ(type, GX_S16);
  EXPECT_EQ(frac, 14);

  GXGetVtxAttrFmtv(GX_VTXFMT2, vat);
  EXPECT_EQ(vat[GX_VA_NRM - GX_VA_POS].attr, GX_VA_NRM);
  EXPECT_EQ(vat[GX_VA_NRM - GX_VA_POS].cnt, GX_NRM_NBT3);
  EXPECT_EQ(vat[GX_VA_NRM - GX_VA_POS].type, GX_S16);
  EXPECT_EQ(vat[GX_VA_TEX4 - GX_VA_POS].attr, GX_VA_TEX4);
  EXPECT_EQ(vat[GX_VA_TEX4 - GX_VA_POS].type, GX_U16);
  EXPECT_EQ(vat[GX_VA_TEX4 - GX_VA_POS].frac, 11);
  EXPECT_EQ(vat[12].attr, GX_VA_NULL);
}

// --- GXSetArray (Aurora array-base command + CP stride command) ---

TEST_F(GXFifoTest, SetArray_Pos_EncodesAuroraArrayBaseAndStride) {
  u8 posData[32]{};
  u8 oldData[8]{};

  GXSetArray(GX_VA_POS, posData, sizeof(posData), 12, false);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 22u);
  EXPECT_EQ(bytes[0], GX_LOAD_AURORA);
  EXPECT_EQ(bytes[1], 0x00);
  EXPECT_EQ(bytes[2], GX_LOAD_AURORA_ARRAYBASE);

  const auto expect_be64 = [&](size_t offset, u64 value) {
    for (size_t i = 0; i < 8; ++i) {
      EXPECT_EQ(bytes[offset + i], static_cast<u8>((value >> (56 - i * 8)) & 0xFF));
    }
  };
  const auto expect_be32 = [&](size_t offset, u32 value) {
    for (size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(bytes[offset + i], static_cast<u8>((value >> (24 - i * 8)) & 0xFF));
    }
  };

  expect_be64(3, static_cast<u64>(reinterpret_cast<uintptr_t>(posData)));
  expect_be32(11, sizeof(posData));
  EXPECT_EQ(bytes[15], 0);
  EXPECT_EQ(bytes[16], GX_LOAD_CP_REG);
  EXPECT_EQ(bytes[17], GX_CP_REG_ARRAYSTRIDE);
  expect_be32(18, 12);

  reset_gx_state();
  gxState().arrays[GX_VA_POS].data = oldData;
  gxState().arrays[GX_VA_POS].size = sizeof(oldData);
  gxState().arrays[GX_VA_POS].stride = 2;
  gxState().arrays[GX_VA_POS].cachedRange.offset = 4;
  gxState().arrays[GX_VA_POS].cachedRange.size = 8;
  gxState().stateDirty = false;
  decode_fifo(bytes);

  EXPECT_EQ(gxState().arrays[GX_VA_POS].data, posData);
  EXPECT_EQ(gxState().arrays[GX_VA_POS].size, sizeof(posData));
  EXPECT_EQ(gxState().arrays[GX_VA_POS].stride, 12);
  EXPECT_FALSE(gxState().arrays[GX_VA_POS].le);
  EXPECT_EQ(gxState().arrays[GX_VA_POS].cachedRange.offset, 0u);
  EXPECT_EQ(gxState().arrays[GX_VA_POS].cachedRange.size, 0u);
  EXPECT_TRUE(gxState().stateDirty);
}

TEST_F(GXFifoTest, SetArray_Nbt_UsesNrmCommandSlotAndState) {
  u8 nbtData[96]{};
  u8 untouchedData[24]{};

  GXSetArray(GX_VA_NBT, nbtData, sizeof(nbtData), 36, false);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 22u);
  EXPECT_EQ(bytes[0], GX_LOAD_AURORA);
  EXPECT_EQ(bytes[1], 0x00);
  EXPECT_EQ(bytes[2], GX_LOAD_AURORA_ARRAYBASE | 0x01);
  EXPECT_EQ(bytes[15], 0);
  EXPECT_EQ(bytes[16], GX_LOAD_CP_REG);
  EXPECT_EQ(bytes[17], GX_CP_REG_ARRAYSTRIDE | 0x01);

  reset_gx_state();
  gxState().arrays[GX_VA_NRM].cachedRange.offset = 12;
  gxState().arrays[GX_VA_NRM].cachedRange.size = 48;
  gxState().arrays[GX_VA_NBT].data = untouchedData;
  gxState().arrays[GX_VA_NBT].size = sizeof(untouchedData);
  gxState().arrays[GX_VA_NBT].stride = 24;
  gxState().stateDirty = false;
  decode_fifo(bytes);

  EXPECT_EQ(gxState().arrays[GX_VA_NRM].data, nbtData);
  EXPECT_EQ(gxState().arrays[GX_VA_NRM].size, sizeof(nbtData));
  EXPECT_EQ(gxState().arrays[GX_VA_NRM].stride, 36);
  EXPECT_FALSE(gxState().arrays[GX_VA_NRM].le);
  EXPECT_EQ(gxState().arrays[GX_VA_NRM].cachedRange.offset, 0u);
  EXPECT_EQ(gxState().arrays[GX_VA_NRM].cachedRange.size, 0u);
  EXPECT_TRUE(gxState().stateDirty);

  EXPECT_EQ(gxState().arrays[GX_VA_NBT].data, untouchedData);
  EXPECT_EQ(gxState().arrays[GX_VA_NBT].size, sizeof(untouchedData));
  EXPECT_EQ(gxState().arrays[GX_VA_NBT].stride, 24);
}

TEST_F(GXFifoTest, SetArray_LittleEndianFlag_UpdatesStateAndClearsCachedRange) {
  u8 clrData[16]{};

  GXSetArray(GX_VA_CLR0, clrData, sizeof(clrData), 4, true);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 22u);
  EXPECT_EQ(bytes[0], GX_LOAD_AURORA);
  EXPECT_EQ(bytes[1], 0x00);
  EXPECT_EQ(bytes[2], GX_LOAD_AURORA_ARRAYBASE | (GX_VA_CLR0 - GX_VA_POS));
  EXPECT_EQ(bytes[15], 1);
  EXPECT_EQ(bytes[16], GX_LOAD_CP_REG);
  EXPECT_EQ(bytes[17], GX_CP_REG_ARRAYSTRIDE | (GX_VA_CLR0 - GX_VA_POS));

  reset_gx_state();
  gxState().arrays[GX_VA_CLR0].data = clrData;
  gxState().arrays[GX_VA_CLR0].size = sizeof(clrData);
  gxState().arrays[GX_VA_CLR0].stride = 4;
  gxState().arrays[GX_VA_CLR0].le = false;
  gxState().arrays[GX_VA_CLR0].cachedRange.offset = 3;
  gxState().arrays[GX_VA_CLR0].cachedRange.size = 9;
  gxState().stateDirty = false;
  decode_fifo(bytes);

  EXPECT_EQ(gxState().arrays[GX_VA_CLR0].data, clrData);
  EXPECT_EQ(gxState().arrays[GX_VA_CLR0].size, sizeof(clrData));
  EXPECT_EQ(gxState().arrays[GX_VA_CLR0].stride, 4);
  EXPECT_TRUE(gxState().arrays[GX_VA_CLR0].le);
  EXPECT_EQ(gxState().arrays[GX_VA_CLR0].cachedRange.offset, 0u);
  EXPECT_EQ(gxState().arrays[GX_VA_CLR0].cachedRange.size, 0u);
  EXPECT_TRUE(gxState().stateDirty);
}

TEST_F(GXFifoTest, InvalidateVtxCache_DropsAllIndexedArrayUploads) {
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    gxState().arrays[i].cachedRange.offset = static_cast<u32>(i * 32);
    gxState().arrays[i].cachedRange.size = static_cast<u32>(i + 1);
  }

  const std::vector<u8> command{GX_CMD_INVL_VC};
  decode_fifo(command);

  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    EXPECT_EQ(gxState().arrays[i].cachedRange.offset, 0u);
    EXPECT_EQ(gxState().arrays[i].cachedRange.size, 0u);
  }
}

TEST_F(GXFifoTest, LoadTexObj_EncodesSdkBpBurstAndAuroraMetadata) {
  alignas(32) u8 image[64]{};
  GXTexObj obj{};
  GXInitTexObj(&obj, image, 8, 8, GX_TF_RGB5A3, GX_REPEAT, GX_MIRROR, GX_FALSE);

  GXLoadTexObj(&obj, GX_TEXMAP2);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_bp_write(bytes, 0x82));
  EXPECT_TRUE(has_bp_write(bytes, 0x86));
  EXPECT_TRUE(has_bp_write(bytes, 0x8A));
  EXPECT_TRUE(has_bp_write(bytes, 0x8E));
  EXPECT_TRUE(has_bp_write(bytes, 0x92));
  EXPECT_TRUE(has_bp_write(bytes, 0x96));
  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_TEXOBJ));

  reset_gx_state();
  decode_fifo(bytes);

  const auto& slot = gxState().loadedTextures[GX_TEXMAP2];
  EXPECT_EQ(slot.data, image);
  EXPECT_EQ(slot.width(), 8u);
  EXPECT_EQ(slot.height(), 8u);
  EXPECT_EQ(slot.format(), GX_TF_RGB5A3);
  EXPECT_FALSE(slot.has_mips());
  EXPECT_EQ(slot.mode0 >> 24, 0x82u);
  EXPECT_EQ(slot.mode1 >> 24, 0x86u);
  EXPECT_EQ(slot.image0 >> 24, 0x8Au);
  EXPECT_EQ(slot.image3 >> 24, 0x96u);
  EXPECT_NE(slot.texObjId, 0u);
  EXPECT_EQ(slot.texDataVersion, 1u);
}

TEST(GXTextureMipCount, ClampsLodToTheDimensionsFullMipChain) {
  alignas(32) u8 image[64]{};

  GXTexObj small{};
  GXInitTexObj(&small, image, 8, 8, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_TRUE);
  GXInitTexObjMaxLOD(&small, 10.0f);
  const auto& smallRef = reinterpret_cast<const GXTexObj_&>(small);
  EXPECT_EQ(smallRef.mip_count(), 4u);

  GXTexObj large{};
  GXInitTexObj(&large, image, 1024, 1024, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_TRUE);
  GXInitTexObjMaxLOD(&large, 10.0f);
  const auto& largeRef = reinterpret_cast<const GXTexObj_&>(large);
  EXPECT_EQ(largeRef.mip_count(), 11u);
}

TEST_F(GXFifoTest, LoadTexObjPcFormat_PreservesFullFormatMetadata) {
  alignas(32) u8 image[64]{};
  GXTexObj obj{};
  GXInitTexObj(&obj, image, 8, 8, GX_TF_RGBA8_PC, GX_REPEAT, GX_REPEAT, GX_FALSE);

  EXPECT_EQ(GXGetTexObjFmt(&obj), GX_TF_RGBA8_PC);

  GXLoadTexObj(&obj, GX_TEXMAP3);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_TEXOBJ));

  reset_gx_state();
  decode_fifo(bytes);

  const auto& slot = gxState().loadedTextures[GX_TEXMAP3];
  EXPECT_EQ(slot.width(), 8u);
  EXPECT_EQ(slot.height(), 8u);
  EXPECT_EQ(slot.format(), GX_TF_RGBA8_PC);
  EXPECT_EQ(slot.raw_format(), static_cast<u32>(GX_TF_RGBA8));
}

TEST_F(GXFifoTest, RawDrawDrainsQueuedMaterialStateWithoutDeferredDirtyBits) {
  __GXSetDirtyState();
  aurora::gx::fifo::clear_buffer();

  constexpr GXColor color = {200, 100, 50, 255};
  GXSetTevColor(GX_TEVREG0, color);

  // TEV register writes are complete FIFO commands, not deferred SDK state.
  // They therefore leave dirtyState clear while still preceding the draw.
  ASSERT_EQ(__gx->dirtyState, 0u);
  ASSERT_GT(aurora::gx::fifo::get_buffer_size(), 0u);

  // The raw bridge receives vertices that are already packed according to the
  // active VAT. Seed its size cache so this test can isolate FIFO ordering.
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  const std::array<u8, 4> vertices{};

  ASSERT_TRUE(aurora::gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, vertices.data(), 4,
                                                static_cast<uint32_t>(vertices.size())));
  EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), 0u);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][0], 200.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][1], 100.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][2], 50.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.colorRegs[GX_TEVREG0][3], 255.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, DisplayListCallWhileRecordingInlinesNestedCommands) {
  std::array<u8, 64> outer{};
  const std::array<u8, 3> nested{GX_NOP, GX_NOP, GX_NOP};

  __GXSetDirtyState();
  aurora::gx::fifo::clear_buffer();
  GXBeginDisplayList(outer.data(), static_cast<u32>(outer.size()));
  GXCallDisplayList(nested.data(), static_cast<u32>(nested.size()));
  const u32 bytes = GXEndDisplayList();

  EXPECT_EQ(bytes, 32u);
  EXPECT_TRUE(std::equal(nested.begin(), nested.end(), outer.begin()));
  EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), 0u);
}

TEST_F(GXFifoTest, DirectEfbCopiesDrainQueuedCommands) {
  aurora::gx::fifo::write_u8(GX_NOP);
  ASSERT_GT(aurora::gx::fifo::get_buffer_size(), 0u);
  GXCopyDisp(nullptr, GX_FALSE);
  EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), 0u);

  std::array<u8, 64> copyDest{};
  aurora::gx::fifo::write_u8(GX_NOP);
  ASSERT_GT(aurora::gx::fifo::get_buffer_size(), 0u);
  GXCopyTex(copyDest.data(), GX_FALSE);
  EXPECT_EQ(aurora::gx::fifo::get_buffer_size(), 0u);
}

TEST_F(GXFifoTest, RawDrawPreservesHorizontalXzQuadVertices) {
  __GXSetDirtyState();
  aurora::gx::fifo::clear_buffer();
  aurora::gfx::testing::use_real_vertex_format_helpers(true);

  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 14;
  g_gxState.vtxDesc[GX_VA_POS] = GX_DIRECT;
  g_gxState.vtxDesc[GX_VA_TEX0] = GX_DIRECT;
  auto& fmt = g_gxState.vtxFmts[GX_VTXFMT0];
  fmt.attrs[GX_VA_POS].cnt = GX_POS_XYZ;
  fmt.attrs[GX_VA_POS].type = GX_F32;
  fmt.attrs[GX_VA_TEX0].cnt = GX_TEX_ST;
  fmt.attrs[GX_VA_TEX0].type = GX_U8;

  std::vector<u8> vertices;
  const auto append_f32_be = [&](float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    vertices.push_back(static_cast<u8>(bits >> 24));
    vertices.push_back(static_cast<u8>(bits >> 16));
    vertices.push_back(static_cast<u8>(bits >> 8));
    vertices.push_back(static_cast<u8>(bits));
  };
  const auto append_vertex = [&](float x, float y, float z, u8 s, u8 t) {
    append_f32_be(x);
    append_f32_be(y);
    append_f32_be(z);
    vertices.push_back(s);
    vertices.push_back(t);
  };

  append_vertex(-53.659431f, 0.27f, -53.659435f, 0, 1);
  append_vertex(-53.659431f, 0.27f, 53.659435f, 0, 0);
  append_vertex(53.659431f, 0.27f, 53.659435f, 1, 0);
  append_vertex(53.659431f, 0.27f, -53.659435f, 1, 1);
  const auto expected = vertices;

  ASSERT_TRUE(aurora::gx::fifo::submit_raw_draw(
      GX_QUADS, GX_VTXFMT0, vertices.data(), 4, static_cast<uint32_t>(vertices.size())));
  EXPECT_EQ(aurora::gfx::testing::last_pushed_vertices(), expected);
}

static void append_test_draw(std::vector<u8>& fifo, GXPrimitive primitive, u16 count) {
  fifo.push_back(static_cast<u8>(primitive) | static_cast<u8>(GX_VTXFMT0));
  fifo.push_back(static_cast<u8>(count >> 8));
  fifo.push_back(static_cast<u8>(count));
  for (u16 vertex = 0; vertex < count; ++vertex) {
    fifo.push_back(static_cast<u8>(vertex));
  }
}

TEST_F(GXFifoTest, DrawTopologyTemplatesPreserveExactGxIndexOrder) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.stateDirty = true;

  const auto decodeAndReadIndices = [&](GXPrimitive primitive, u16 count) {
    std::vector<u8> fifo;
    append_test_draw(fifo, primitive, count);
    decode_fifo(fifo);
    return aurora::gfx::testing::last_pushed_indices();
  };

  EXPECT_EQ(decodeAndReadIndices(GX_QUADS, 8),
            (std::vector<u16>{0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4}));
  g_gxState.stateDirty = true;
  EXPECT_EQ(decodeAndReadIndices(GX_TRIANGLES, 6),
            (std::vector<u16>{0, 1, 2, 3, 4, 5}));
  g_gxState.stateDirty = true;
  EXPECT_EQ(decodeAndReadIndices(GX_TRIANGLEFAN, 5),
            (std::vector<u16>{0, 1, 2, 0, 2, 3, 0, 3, 4}));
  g_gxState.stateDirty = true;
  EXPECT_EQ(decodeAndReadIndices(GX_TRIANGLEFAN, 2),
            (std::vector<u16>{0, 1}));
  g_gxState.stateDirty = true;
  EXPECT_EQ(decodeAndReadIndices(GX_TRIANGLESTRIP, 6),
            (std::vector<u16>{0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5}));
  g_gxState.stateDirty = true;
  EXPECT_TRUE(decodeAndReadIndices(GX_TRIANGLESTRIP, 0).empty());
  g_gxState.stateDirty = true;
  EXPECT_EQ(decodeAndReadIndices(GX_LINES, 2),
            (std::vector<u16>{0, 1, 3, 3, 2, 0}));
}

TEST_F(GXFifoTest, MergedDrawOffsetsCachedTopologyWithoutJoiningPrimitives) {
  g_gxState.lastVtxFmt = GX_VTXFMT0;
  g_gxState.lastVtxSize = 1;
  g_gxState.stateDirty = true;
  aurora::gfx::testing::use_draw_command_tracking(true);

  std::vector<u8> fifo;
  append_test_draw(fifo, GX_TRIANGLES, 3);
  append_test_draw(fifo, GX_TRIANGLES, 3);
  decode_fifo(fifo);

  EXPECT_EQ(aurora::gfx::g_mergedDrawCallCount, 1u);
  EXPECT_EQ(aurora::gfx::testing::last_pushed_indices(),
            (std::vector<u16>{3, 4, 5}));
}

TEST_F(GXFifoTest, TexBufferSize_UsesExactLinearPcFormatSizes) {
  EXPECT_EQ(GXGetTexBufferSize(8, 4, GX_TF_R8_PC, GX_FALSE, 0), 32u);
  EXPECT_EQ(GXGetTexBufferSize(8, 4, GX_TF_RGBA8_PC, GX_FALSE, 0), 128u);
  EXPECT_EQ(GXGetTexBufferSize(8, 4, GX_TF_R8_PC, GX_TRUE, 2), 42u);
  EXPECT_EQ(GXGetTexBufferSize(8, 4, GX_TF_RGBA8_PC, GX_TRUE, 2), 168u);
}

TEST_F(GXFifoTest, InvalidateTexAll_EmitsAuroraCacheInvalidationAndDirtiesState) {
  GXInvalidateTexAll();
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_INVALIDATE_TEX_ALL));

  reset_gx_state();
  gxState().stateDirty = false;
  decode_fifo(bytes);
  EXPECT_TRUE(gxState().stateDirty);
}

TEST_F(GXFifoTest, TexObjRawDimensions_WrapAtTenBitBoundary) {
  auto& slot = gxState().loadedTextures[GX_TEXMAP0];
  slot.image0 = (0x3FFu << 0) | (0x3FFu << 10);
  slot.mWidth = 0;
  slot.mHeight = 0;

  EXPECT_EQ(slot.width(), 0u);
  EXPECT_EQ(slot.height(), 0u);
}

TEST_F(GXFifoTest, TexObjExplicitDimensions_DoNotWrapAtTenBitBoundary) {
  auto& slot = gxState().loadedTextures[GX_TEXMAP0];
  slot.image0 = (0x3FFu << 0) | (0x3FFu << 10);
  slot.mWidth = 1024;
  slot.mHeight = 1024;

  EXPECT_EQ(slot.width(), 1024u);
  EXPECT_EQ(slot.height(), 1024u);
}

TEST_F(GXFifoTest, LoadTexObjCiAndTlut_PopulatesTextureAndTlutSlots) {
  alignas(32) u8 image[64]{};
  alignas(32) u16 palette[16]{};
  GXTexObj texObj{};
  GXTlutObj tlutObj{};

  GXInitTexObjCI(&texObj, image, 8, 8, GX_TF_C4, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT3);
  GXInitTlutObj(&tlutObj, palette, GX_TL_RGB565, 16);

  GXLoadTexObj(&texObj, GX_TEXMAP1);
  GXLoadTlut(&tlutObj, GX_TLUT3);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_bp_write(bytes, 0x81));
  EXPECT_TRUE(has_bp_write(bytes, 0x85));
  EXPECT_TRUE(has_bp_write(bytes, 0x89));
  EXPECT_TRUE(has_bp_write(bytes, 0x8D));
  EXPECT_TRUE(has_bp_write(bytes, 0x91));
  EXPECT_TRUE(has_bp_write(bytes, 0x95));
  EXPECT_TRUE(has_bp_write(bytes, 0x99));
  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_TEXOBJ));
  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_TLUT));

  reset_gx_state();
  decode_fifo(bytes);

  const auto& texSlot = gxState().loadedTextures[GX_TEXMAP1];
  EXPECT_EQ(texSlot.data, image);
  EXPECT_EQ(texSlot.width(), 8u);
  EXPECT_EQ(texSlot.height(), 8u);
  EXPECT_EQ(texSlot.format(), GX_TF_C4);
  EXPECT_EQ(texSlot.tlut, GX_TLUT3);

  const auto& tlutSlot = gxState().loadedTluts[GX_TLUT3];
  EXPECT_EQ(tlutSlot.data, palette);
  EXPECT_EQ(tlutSlot.format, GX_TL_RGB565);
  EXPECT_EQ(tlutSlot.numEntries, 16u);
  EXPECT_NE(tlutSlot.tlutObjId, 0u);
  EXPECT_EQ(tlutSlot.tlutDataVersion, 1u);
}

TEST_F(GXFifoTest, DestroyTexObj_DoesNotEvictTextureDataAndClearsIdentity) {
  alignas(32) u8 image[64]{};
  GXTexObj obj{};
  GXInitTexObj(&obj, image, 8, 8, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
  GXDestroyTexObj(&obj);
  auto bytes = capture_fifo();

  EXPECT_FALSE(has_aurora_cmd(bytes, GX_LOAD_AURORA_DESTROY_TEXOBJ));
  EXPECT_EQ(reinterpret_cast<const GXTexObj_*>(&obj)->texObjId, 0u);

  reset_gx_state();
  decode_fifo(bytes);
}

TEST_F(GXFifoTest, DestroyTexObj_DoesNotPoisonLoadedSlotCache) {
  alignas(32) u8 imageA[64]{};
  alignas(32) u8 imageB[64]{};
  GXTexObj objA{};
  GXTexObj objB{};

  GXInitTexObj(&objA, imageA, 8, 8, GX_TF_RGB5A3, GX_REPEAT, GX_REPEAT, GX_FALSE);
  GXLoadTexObj(&objA, GX_TEXMAP2);
  auto loadABytes = capture_fifo();
  const auto destroyedTexObjId = reinterpret_cast<const GXTexObj_*>(&objA)->texObjId;

  GXDestroyTexObj(&objA);
  auto destroyBytes = capture_fifo();

  GXInitTexObj(&objB, imageB, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
  GXLoadTexObj(&objB, GX_TEXMAP2);
  auto loadBBytes = capture_fifo();

  reset_gx_state();
  decode_fifo(loadABytes);
  auto& slot = gxState().loadedTextures[GX_TEXMAP2];
  EXPECT_EQ(slot.texObjId, destroyedTexObjId);
  EXPECT_FALSE(slot.no_cache());

  decode_fifo(destroyBytes);
  EXPECT_EQ(slot.texObjId, destroyedTexObjId);
  EXPECT_FALSE(slot.no_cache());

  decode_fifo(loadBBytes);
  EXPECT_EQ(slot.data, imageB);
  EXPECT_EQ(slot.format(), GX_TF_RGB565);
  EXPECT_FALSE(slot.no_cache());
}

TEST_F(GXFifoTest, DestroyTlutObj_EmitsAuroraDestroyCommandAndClearsIdentity) {
  alignas(32) u16 palette[16]{};
  GXTlutObj obj{};
  GXInitTlutObj(&obj, palette, GX_TL_RGB565, 16);
  GXDestroyTlutObj(&obj);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_DESTROY_TLUT));
  EXPECT_EQ(reinterpret_cast<const GXTlutObj_*>(&obj)->tlutObjId, 0u);

  reset_gx_state();
  decode_fifo(bytes);
}

TEST_F(GXFifoTest, DestroyTlutObj_MarksLoadedSlotNoCacheUntilReloaded) {
  alignas(32) u16 paletteA[16]{};
  alignas(32) u16 paletteB[16]{};
  GXTlutObj objA{};
  GXTlutObj objB{};

  GXInitTlutObj(&objA, paletteA, GX_TL_RGB565, 16);
  GXLoadTlut(&objA, GX_TLUT3);
  auto loadABytes = capture_fifo();
  const auto destroyedTlutObjId = reinterpret_cast<const GXTlutObj_*>(&objA)->tlutObjId;

  GXDestroyTlutObj(&objA);
  auto destroyBytes = capture_fifo();

  GXInitTlutObj(&objB, paletteB, GX_TL_IA8, 16);
  GXLoadTlut(&objB, GX_TLUT3);
  auto loadBBytes = capture_fifo();

  reset_gx_state();
  decode_fifo(loadABytes);
  auto& slot = gxState().loadedTluts[GX_TLUT3];
  EXPECT_EQ(slot.tlutObjId, destroyedTlutObjId);
  EXPECT_FALSE(slot.no_cache());

  decode_fifo(destroyBytes);
  EXPECT_EQ(slot.tlutObjId, destroyedTlutObjId);
  EXPECT_TRUE(slot.no_cache());

  decode_fifo(loadBBytes);
  EXPECT_EQ(slot.data, paletteB);
  EXPECT_EQ(slot.format, GX_TL_IA8);
  EXPECT_FALSE(slot.no_cache());
}

TEST_F(GXFifoTest, DestroyCopyTex_EmitsAuroraDestroyCommand) {
  alignas(32) u8 image[32]{};

  GXDestroyCopyTex(image);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_DESTROY_COPY_TEX));

  reset_gx_state();
  decode_fifo(bytes);
}

TEST_F(GXFifoTest, DestroyCopyTex_RemovesActiveCopyTextureAndCacheEntriesForPointer) {
  alignas(32) u8 imageA[32]{};
  alignas(32) u8 imageB[32]{};

  const aurora::gx::GXState::CopyTextureRef ref{.revision = 1};
  gxState().copyTextures[imageA] = ref;
  gxState().copyTextures[imageB] = ref;
  gxState().copyTextureCache.emplace(
      aurora::gx::GXState::CopyTextureKey{.dest = imageA, .width = 32, .height = 32, .format = GX_TF_I4}, ref);
  gxState().copyTextureCache.emplace(
      aurora::gx::GXState::CopyTextureKey{.dest = imageA, .width = 64, .height = 64, .format = GX_TF_I8}, ref);
  gxState().copyTextureCache.emplace(
      aurora::gx::GXState::CopyTextureKey{.dest = imageB, .width = 32, .height = 32, .format = GX_TF_I4}, ref);

  GXDestroyCopyTex(imageA);
  auto bytes = capture_fifo();

  decode_fifo(bytes);

  EXPECT_FALSE(gxState().copyTextures.contains(imageA));
  EXPECT_TRUE(gxState().copyTextures.contains(imageB));
  for (const auto& [key, _] : gxState().copyTextureCache) {
    EXPECT_NE(key.dest, imageA);
  }
  EXPECT_EQ(gxState().copyTextureCache.size(), 1u);
}

// BP genMode (requires __GXSetDirtyState() flush)

// --- GXSetCullMode ---

TEST_F(GXFifoTest, CullMode_Back) {
  GXSetCullMode(GX_CULL_BACK);
  auto bytes = flush_and_capture();

  reset_gx_state();
  g_gxState.cullMode = GX_CULL_NONE;
  decode_fifo(bytes);

  // The encoder swaps front/back for hardware, and decoder swaps back
  EXPECT_EQ(g_gxState.cullMode, GX_CULL_BACK);
}

TEST_F(GXFifoTest, CullMode_Front) {
  GXSetCullMode(GX_CULL_FRONT);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.cullMode, GX_CULL_FRONT);
}

TEST_F(GXFifoTest, CullMode_None) {
  GXSetCullMode(GX_CULL_NONE);
  auto bytes = flush_and_capture();

  reset_gx_state();
  g_gxState.cullMode = GX_CULL_BACK;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.cullMode, GX_CULL_NONE);
}

TEST_F(GXFifoTest, GetLinePointCullShadowState) {
  GXSetLineWidth(12, GX_TO_ZERO);
  GXSetPointSize(34, GX_TO_ONE);
  GXSetCullMode(GX_CULL_FRONT);

  u8 lineWidth = 0;
  u8 pointSize = 0;
  GXTexOffset lineOffs = GX_TO_ZERO;
  GXTexOffset pointOffs = GX_TO_ZERO;
  GXCullMode cullMode = GX_CULL_NONE;

  GXGetLineWidth(&lineWidth, &lineOffs);
  GXGetPointSize(&pointSize, &pointOffs);
  GXGetCullMode(&cullMode);

  EXPECT_EQ(lineWidth, 12);
  EXPECT_EQ(lineOffs, GX_TO_ZERO);
  EXPECT_EQ(pointSize, 34);
  EXPECT_EQ(pointOffs, GX_TO_ONE);
  EXPECT_EQ(cullMode, GX_CULL_FRONT);
}

TEST_F(GXFifoTest, LinePointSize_Decode) {
  GXSetLineWidth(12, GX_TO_ZERO);
  GXSetPointSize(34, GX_TO_ONE);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 10u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x22);
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0x22);

  reset_gx_state();
  g_gxState.lineWidth = 1;
  g_gxState.pointSize = 2;
  g_gxState.lineTexOffset = GX_TO_ONE;
  g_gxState.pointTexOffset = GX_TO_ZERO;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.lineWidth, 12u);
  EXPECT_EQ(g_gxState.lineTexOffset, GX_TO_ZERO);
  EXPECT_EQ(g_gxState.pointSize, 34u);
  EXPECT_EQ(g_gxState.pointTexOffset, GX_TO_ONE);
}

// --- GXSetNumTevStages / GXSetNumTexGens / GXSetNumChans ---

TEST_F(GXFifoTest, NumTevStages) {
  GXSetNumTevStages(4);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.numTevStages, 4u);
}

TEST_F(GXFifoTest, NumTexGens) {
  GXSetNumTexGens(3);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.numTexGens, 3u);
}

TEST_F(GXFifoTest, NumChans) {
  GXSetNumChans(2);
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.numChans, 2u);
}

// XF registers (direct FIFO writes)

// --- GXLoadPosMtxImm (XF 0x000-0x077) ---

TEST_F(GXFifoTest, LoadPosMtxImm_Identity) {
  // 3x4 identity matrix
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;
  mtx.m2[2] = 1.0f;

  GXLoadPosMtxImm(&mtx, GX_PNMTX0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.pnMtx[0].pos;
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadPosMtxImm_Translation) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;
  mtx.m2[2] = 1.0f;
  mtx.m0[3] = 10.0f;
  mtx.m1[3] = 20.0f;
  mtx.m2[3] = 30.0f;

  GXLoadPosMtxImm(&mtx, GX_PNMTX3);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.pnMtx[3].pos;
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 10.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 20.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 30.0f);
}

// --- GXLoadNrmMtxImm (XF 0x400-0x459) ---

TEST_F(GXFifoTest, LoadNrmMtxImm_Identity) {
  // 3x4 matrix with 3x3 identity (translation column ignored by encoder)
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;
  mtx.m2[2] = 1.0f;

  GXLoadNrmMtxImm(&mtx, GX_PNMTX0);
  auto bytes = capture_fifo();

  // XF opcode 0x10
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x10);

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.pnMtx[0].nrm;
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.0f);
}

TEST_F(GXFifoTest, LoadNrmMtxImm_ArbitraryValues) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 0.5f;  mtx.m0[1] = -0.5f; mtx.m0[2] = 0.7f;  mtx.m0[3] = 999.0f;
  mtx.m1[0] = 0.3f;  mtx.m1[1] = 0.8f;  mtx.m1[2] = -0.1f; mtx.m1[3] = 888.0f;
  mtx.m2[0] = -0.6f; mtx.m2[1] = 0.2f;  mtx.m2[2] = 0.9f;  mtx.m2[3] = 777.0f;

  GXLoadNrmMtxImm(&mtx, GX_PNMTX0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.pnMtx[0].nrm;
  // 3x3 portion should round-trip
  EXPECT_FLOAT_EQ(decoded.m0[0], 0.5f);
  EXPECT_FLOAT_EQ(decoded.m0[1], -0.5f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.7f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.3f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 0.8f);
  EXPECT_FLOAT_EQ(decoded.m1[2], -0.1f);
  EXPECT_FLOAT_EQ(decoded.m2[0], -0.6f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.2f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 0.9f);
  // Translation column is NOT written by the encoder, so it stays zeroed
  EXPECT_FLOAT_EQ(decoded.m0[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadNrmMtxImm_DifferentSlot) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 2.0f;
  mtx.m1[1] = 3.0f;
  mtx.m2[2] = 4.0f;

  GXLoadNrmMtxImm(&mtx, GX_PNMTX3);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.pnMtx[3].nrm;
  EXPECT_FLOAT_EQ(decoded.m0[0], 2.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 3.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 4.0f);
}

TEST_F(GXFifoTest, LoadNrmMtxImm_Isolation) {
  // Loading nrm into slot 0 should not affect slot 1 or the pos matrix
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 11.0f;
  mtx.m1[1] = 22.0f;
  mtx.m2[2] = 33.0f;

  GXLoadNrmMtxImm(&mtx, GX_PNMTX0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Slot 0 nrm should have our values
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].nrm.m0[0], 11.0f);
  // Slot 0 pos should remain zeroed (nrm write doesn't touch pos)
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].pos.m0[0], 0.0f);
  // Slot 1 nrm should remain zeroed
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[1].nrm.m0[0], 0.0f);
}

TEST_F(GXFifoTest, LoadNrmMtxImm_WithPosMtx) {
  // Load both pos and nrm into the same slot, verify both decode correctly
  aurora::Mat3x4<float> posMtx{};
  posMtx.m0[0] = 1.0f;
  posMtx.m1[1] = 1.0f;
  posMtx.m2[2] = 1.0f;
  posMtx.m0[3] = 5.0f;
  posMtx.m1[3] = 10.0f;
  posMtx.m2[3] = 15.0f;

  aurora::Mat3x4<float> nrmMtx{};
  nrmMtx.m0[0] = 0.5f;
  nrmMtx.m1[1] = 0.5f;
  nrmMtx.m2[2] = 0.5f;

  GXLoadPosMtxImm(&posMtx, GX_PNMTX0);
  GXLoadNrmMtxImm(&nrmMtx, GX_PNMTX0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Position matrix
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].pos.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].pos.m0[3], 5.0f);
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].pos.m1[3], 10.0f);
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].pos.m2[3], 15.0f);
  // Normal matrix
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].nrm.m0[0], 0.5f);
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].nrm.m1[1], 0.5f);
  EXPECT_FLOAT_EQ(g_gxState.pnMtx[0].nrm.m2[2], 0.5f);
}

// --- GXLoadTexMtxImm 3x4 (XF 0x078-0x0EF) ---

TEST_F(GXFifoTest, LoadTexMtx3x4_Identity) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;
  mtx.m2[2] = 1.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x10);

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[0];
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadTexMtx3x4_ArbitraryValues) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 2.0f;  mtx.m0[1] = 0.5f;  mtx.m0[2] = 0.0f;  mtx.m0[3] = 10.0f;
  mtx.m1[0] = -0.5f; mtx.m1[1] = 3.0f;  mtx.m1[2] = 0.0f;  mtx.m1[3] = 20.0f;
  mtx.m2[0] = 0.0f;  mtx.m2[1] = 0.0f;  mtx.m2[2] = 1.5f;  mtx.m2[3] = -5.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[0];
  EXPECT_FLOAT_EQ(decoded.m0[0], 2.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.5f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 10.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], -0.5f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 3.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 20.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.5f);
  EXPECT_FLOAT_EQ(decoded.m2[3], -5.0f);
}

TEST_F(GXFifoTest, LoadTexMtx3x4_DifferentSlot) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 7.0f;
  mtx.m1[1] = 8.0f;
  mtx.m2[2] = 9.0f;
  mtx.m2[3] = 42.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX5, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // GX_TEXMTX5 = 45, addr = 45*4 = 180 = 0xB4, index = (0xB4 - 0x78) / 12 = 5
  auto& decoded = g_gxState.texMtxs[5];
  EXPECT_FLOAT_EQ(decoded.m0[0], 7.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 8.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 9.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 42.0f);
}

TEST_F(GXFifoTest, LoadTexMtx3x4_LastSlot) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 11.0f;
  mtx.m1[1] = 22.0f;
  mtx.m2[2] = 33.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX9, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[9];
  EXPECT_FLOAT_EQ(decoded.m0[0], 11.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 22.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 33.0f);
}

TEST_F(GXFifoTest, LoadTexMtx3x4_Isolation) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 50.0f;
  mtx.m1[1] = 60.0f;
  mtx.m2[2] = 70.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FLOAT_EQ(g_gxState.texMtxs[0].m0[0], 50.0f);
  // Slot 1 should remain zeroed
  EXPECT_FLOAT_EQ(g_gxState.texMtxs[1].m0[0], 0.0f);
  EXPECT_FLOAT_EQ(g_gxState.texMtxs[1].m1[1], 0.0f);
}

// --- GXLoadTexMtxImm 2x4 (XF 0x078-0x0EF) ---

TEST_F(GXFifoTest, LoadTexMtx2x4_Identity) {
  // 2x4 identity: row0 = [1,0,0,0], row1 = [0,1,0,0]
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX2x4);
  auto bytes = capture_fifo();

  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x10);

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[0];
  // First two rows should round-trip
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 0.0f);
  // Third row not written by 2x4, should be zeroed
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadTexMtx2x4_ArbitraryValues) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 0.5f;  mtx.m0[1] = -1.0f; mtx.m0[2] = 0.25f; mtx.m0[3] = 100.0f;
  mtx.m1[0] = 3.0f;  mtx.m1[1] = 0.0f;  mtx.m1[2] = -2.5f; mtx.m1[3] = -50.0f;
  // Row 2 values should be ignored by the encoder
  mtx.m2[0] = 999.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX2x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[0];
  EXPECT_FLOAT_EQ(decoded.m0[0], 0.5f);
  EXPECT_FLOAT_EQ(decoded.m0[1], -1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.25f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 100.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 3.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], -2.5f);
  EXPECT_FLOAT_EQ(decoded.m1[3], -50.0f);
  // Row 2 should be zeroed (only 8 floats written)
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
}

TEST_F(GXFifoTest, LoadTexMtx2x4_DifferentSlot) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 4.0f;
  mtx.m0[3] = 15.0f;
  mtx.m1[1] = 5.0f;
  mtx.m1[3] = 25.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX3, GX_MTX2x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.texMtxs[3];
  EXPECT_FLOAT_EQ(decoded.m0[0], 4.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 15.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 5.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 25.0f);
}

TEST_F(GXFifoTest, LoadTexMtx2x4_Isolation) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 10.0f;
  mtx.m1[1] = 20.0f;

  GXLoadTexMtxImm(&mtx, GX_TEXMTX0, GX_MTX2x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FLOAT_EQ(g_gxState.texMtxs[0].m0[0], 10.0f);
  EXPECT_FLOAT_EQ(g_gxState.texMtxs[0].m1[1], 20.0f);
  // Slot 1 should remain zeroed
  EXPECT_FLOAT_EQ(g_gxState.texMtxs[1].m0[0], 0.0f);
  EXPECT_FLOAT_EQ(g_gxState.texMtxs[1].m1[1], 0.0f);
}

// --- GXSetProjection (XF 0x1020-0x1026) ---

TEST_F(GXFifoTest, Projection_Perspective) {
  aurora::Mat4x4<float> proj{};
  proj.m0[0] = 1.5f; // near / (right - left) * 2
  proj.m0[2] = 0.1f;
  proj.m1[1] = 2.0f; // near / (top - bottom) * 2
  proj.m1[2] = 0.2f;
  proj.m2[2] = -1.002f;
  proj.m2[3] = -0.2002f;
  proj.m3[2] = -1.0f;

  GXSetProjection(&proj, GX_PERSPECTIVE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.projType, GX_PERSPECTIVE);
  EXPECT_FLOAT_EQ(g_gxState.proj.m0[0], 1.5f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m0[2], 0.1f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m1[1], 2.0f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m1[2], 0.2f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m2[2], -1.002f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m2[3], -0.2002f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m3[2], -1.0f);
}

TEST_F(GXFifoTest, Projection_Orthographic) {
  aurora::Mat4x4<float> proj{};
  proj.m0[0] = 2.0f / 640.0f;
  proj.m0[3] = -1.0f;
  proj.m1[1] = 2.0f / 480.0f;
  proj.m1[3] = -1.0f;
  proj.m2[2] = -1.0f / 10000.0f;
  proj.m2[3] = 0.0f;
  proj.m3[3] = 1.0f;

  GXSetProjection(&proj, GX_ORTHOGRAPHIC);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.projType, GX_ORTHOGRAPHIC);
  EXPECT_FLOAT_EQ(g_gxState.proj.m0[0], 2.0f / 640.0f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m0[3], -1.0f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m1[1], 2.0f / 480.0f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m1[3], -1.0f);
  EXPECT_FLOAT_EQ(g_gxState.proj.m3[3], 1.0f);
}

TEST_F(GXFifoTest, GetProjectionAndScissorShadowState) {
  const f32 proj[] = {0.0f, 1.5f, 0.1f, 2.0f, 0.2f, -1.002f, -0.2002f};
  f32 outProj[7]{};
  u32 left = 0, top = 0, width = 0, height = 0;

  GXSetProjectionv(proj);
  GXSetScissor(16, 24, 320, 240);
  GXGetProjectionv(outProj);
  GXGetScissor(&left, &top, &width, &height);

  for (size_t i = 0; i < 7; ++i) {
    EXPECT_FLOAT_EQ(outProj[i], proj[i]);
  }
  EXPECT_EQ(left, 16u);
  EXPECT_EQ(top, 24u);
  EXPECT_EQ(width, 320u);
  EXPECT_EQ(height, 240u);
}

TEST_F(GXFifoTest, Scissor_EncodesBpAndDecodesLogicalState) {
  GXSetScissor(16, 24, 320, 240);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_bp_write(bytes, 0x20));
  EXPECT_TRUE(has_bp_write(bytes, 0x21));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.logicalScissor.x, 16);
  EXPECT_EQ(g_gxState.logicalScissor.y, 24);
  EXPECT_EQ(g_gxState.logicalScissor.width, 320);
  EXPECT_EQ(g_gxState.logicalScissor.height, 240);
}

TEST_F(GXFifoTest, ScissorBoxOffset_EncodesBp59AndDecodesState) {
  GXSetScissorBoxOffset(1024, 0);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_bp_write(bytes, 0x59));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.scissorOffsetX, 1024);
  EXPECT_EQ(g_gxState.scissorOffsetY, 0);
}

TEST_F(GXFifoTest, GetViewportShadowState) {
  f32 vp[6]{};

  GXSetViewport(10.0f, 20.0f, 640.0f, 480.0f, 0.1f, 1.0f);
  GXGetViewportv(vp);
  EXPECT_FLOAT_EQ(vp[0], 10.0f);
  EXPECT_FLOAT_EQ(vp[1], 20.0f);
  EXPECT_FLOAT_EQ(vp[2], 640.0f);
  EXPECT_FLOAT_EQ(vp[3], 480.0f);
  EXPECT_FLOAT_EQ(vp[4], 0.1f);
  EXPECT_FLOAT_EQ(vp[5], 1.0f);

  GXSetViewportJitter(30.0f, 40.0f, 320.0f, 240.0f, 0.2f, 0.9f, 0);
  GXGetViewportv(vp);
  EXPECT_FLOAT_EQ(vp[0], 30.0f);
  EXPECT_FLOAT_EQ(vp[1], 39.5f);
  EXPECT_FLOAT_EQ(vp[2], 320.0f);
  EXPECT_FLOAT_EQ(vp[3], 240.0f);
  EXPECT_FLOAT_EQ(vp[4], 0.2f);
  EXPECT_FLOAT_EQ(vp[5], 0.9f);
}

TEST_F(GXFifoTest, Viewport_DecodesLogicalViewportState) {
  GXSetViewport(10.0f, 20.0f, 640.0f, 480.0f, 0.1f, 1.0f);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FLOAT_EQ(g_gxState.logicalViewport.left, 10.0f);
  EXPECT_FLOAT_EQ(g_gxState.logicalViewport.top, 20.0f);
  EXPECT_FLOAT_EQ(g_gxState.logicalViewport.width, 640.0f);
  EXPECT_FLOAT_EQ(g_gxState.logicalViewport.height, 480.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.left, 10.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.top, 20.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.width, 640.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.height, 480.0f);
}

TEST_F(GXFifoTest, ViewportRender_EncodesAuroraOverride) {
  GXSetViewportRender(100.0f, 50.0f, 1280.0f, 720.0f, 0.0f, 1.0f);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_VIEWPORT_RENDER));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FLOAT_EQ(g_gxState.renderViewport.left, 100.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.top, 50.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.width, 1280.0f);
  EXPECT_FLOAT_EQ(g_gxState.renderViewport.height, 720.0f);
}

TEST_F(GXFifoTest, ScissorRender_EncodesAuroraOverride) {
  GXSetScissorRender(100, 40, 800, 600);
  auto bytes = capture_fifo();

  EXPECT_TRUE(has_aurora_cmd(bytes, GX_LOAD_AURORA_SCISSOR_RENDER));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.renderScissor.x, 100);
  EXPECT_EQ(g_gxState.renderScissor.y, 40);
  EXPECT_EQ(g_gxState.renderScissor.width, 800);
  EXPECT_EQ(g_gxState.renderScissor.height, 600);
}

// --- GXLoadLightObjImm (XF 0x600-0x67F) ---

TEST_F(GXFifoTest, LoadLightObjImm_Light0_BasicColor) {
  GXLightObj lightObj;
  GXInitLightPos(&lightObj, 100.0f, 200.0f, 300.0f);
  GXInitLightDir(&lightObj, 0.0f, -1.0f, 0.0f);
  GXInitLightColor(&lightObj, {255, 128, 64, 255});
  GXInitLightAttn(&lightObj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

  GXLoadLightObjImm(&lightObj, GX_LIGHT0);
  auto bytes = capture_fifo();

  // XF bulk write: opcode 0x10
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x10);

  reset_gx_state();
  g_gxState.preparedLightsDirty = false;
  decode_fifo(bytes);
  EXPECT_TRUE(g_gxState.preparedLightsDirty);

  auto& light = g_gxState.lights[0];
  // Color
  EXPECT_NEAR(light.color[0], 255.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[1], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[2], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[3], 255.f / 255.f, 1.f / 255.f);
  // Position
  EXPECT_FLOAT_EQ(light.pos[0], 100.0f);
  EXPECT_FLOAT_EQ(light.pos[1], 200.0f);
  EXPECT_FLOAT_EQ(light.pos[2], 300.0f);
  // Direction (GXInitLightDir negates)
  EXPECT_FLOAT_EQ(light.dir[0], 0.0f);
  EXPECT_FLOAT_EQ(light.dir[1], 1.0f);
  EXPECT_FLOAT_EQ(light.dir[2], 0.0f);
  // Cosine attenuation
  EXPECT_FLOAT_EQ(light.cosAtt[0], 1.0f);
  EXPECT_FLOAT_EQ(light.cosAtt[1], 0.0f);
  EXPECT_FLOAT_EQ(light.cosAtt[2], 0.0f);
  // Distance attenuation
  EXPECT_FLOAT_EQ(light.distAtt[0], 1.0f);
  EXPECT_FLOAT_EQ(light.distAtt[1], 0.0f);
  EXPECT_FLOAT_EQ(light.distAtt[2], 0.0f);
}

TEST_F(GXFifoTest, LoadLightObjImm_Light3_Attenuation) {
  GXLightObj lightObj;
  GXInitLightPos(&lightObj, -50.0f, 0.0f, 75.0f);
  GXInitLightDir(&lightObj, 1.0f, 0.0f, 0.0f);
  GXInitLightColor(&lightObj, {0, 255, 0, 128});
  GXInitLightAttn(&lightObj, 0.5f, 0.3f, 0.2f, 1.0f, 0.01f, 0.001f);

  GXLoadLightObjImm(&lightObj, GX_LIGHT3);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& light = g_gxState.lights[3];
  EXPECT_NEAR(light.color[0], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[1], 255.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[2], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[3], 128.f / 255.f, 1.f / 255.f);
  EXPECT_FLOAT_EQ(light.pos[0], -50.0f);
  EXPECT_FLOAT_EQ(light.pos[1], 0.0f);
  EXPECT_FLOAT_EQ(light.pos[2], 75.0f);
  EXPECT_FLOAT_EQ(light.dir[0], -1.0f);
  EXPECT_FLOAT_EQ(light.dir[1], 0.0f);
  EXPECT_FLOAT_EQ(light.dir[2], 0.0f);
  EXPECT_FLOAT_EQ(light.cosAtt[0], 0.5f);
  EXPECT_FLOAT_EQ(light.cosAtt[1], 0.3f);
  EXPECT_FLOAT_EQ(light.cosAtt[2], 0.2f);
  EXPECT_FLOAT_EQ(light.distAtt[0], 1.0f);
  EXPECT_FLOAT_EQ(light.distAtt[1], 0.01f);
  EXPECT_FLOAT_EQ(light.distAtt[2], 0.001f);
}

TEST_F(GXFifoTest, LoadLightObjImm_Light7_LastLight) {
  GXLightObj lightObj;
  GXInitLightPos(&lightObj, 0.0f, 1000.0f, 0.0f);
  GXInitLightDir(&lightObj, 0.0f, 0.0f, -1.0f);
  GXInitLightColor(&lightObj, {128, 128, 128, 255});
  GXInitLightAttn(&lightObj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

  GXLoadLightObjImm(&lightObj, GX_LIGHT7);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& light = g_gxState.lights[7];
  EXPECT_NEAR(light.color[0], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[1], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[2], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(light.color[3], 255.f / 255.f, 1.f / 255.f);
  EXPECT_FLOAT_EQ(light.pos[0], 0.0f);
  EXPECT_FLOAT_EQ(light.pos[1], 1000.0f);
  EXPECT_FLOAT_EQ(light.pos[2], 0.0f);
  EXPECT_FLOAT_EQ(light.dir[0], 0.0f);
  EXPECT_FLOAT_EQ(light.dir[1], 0.0f);
  EXPECT_FLOAT_EQ(light.dir[2], 1.0f);
}

TEST_F(GXFifoTest, LoadLightObjImm_SpotLight) {
  GXLightObj lightObj;
  GXInitLightPos(&lightObj, 10.0f, 20.0f, 30.0f);
  GXInitLightDir(&lightObj, 0.0f, -1.0f, 0.0f);
  GXInitLightSpot(&lightObj, 45.0f, GX_SP_COS);
  GXInitLightDistAttn(&lightObj, 100.0f, 0.5f, GX_DA_MEDIUM);
  GXInitLightColor(&lightObj, {255, 255, 255, 255});

  GXLoadLightObjImm(&lightObj, GX_LIGHT1);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& light = g_gxState.lights[1];
  EXPECT_NEAR(light.color[0], 1.0f, 1.f / 255.f);
  EXPECT_NEAR(light.color[1], 1.0f, 1.f / 255.f);
  EXPECT_NEAR(light.color[2], 1.0f, 1.f / 255.f);
  EXPECT_FLOAT_EQ(light.pos[0], 10.0f);
  EXPECT_FLOAT_EQ(light.pos[1], 20.0f);
  EXPECT_FLOAT_EQ(light.pos[2], 30.0f);
  // GX_SP_COS with cutoff=45: cr = cos(45 * pi / 180)
  // a0 = -cr/(1-cr), a1 = 1/(1-cr), a2 = 0
  float cr = std::cos(45.0f * M_PIF / 180.0f);
  EXPECT_FLOAT_EQ(light.cosAtt[0], -cr / (1.0f - cr));
  EXPECT_FLOAT_EQ(light.cosAtt[1], 1.0f / (1.0f - cr));
  EXPECT_FLOAT_EQ(light.cosAtt[2], 0.0f);
  // GX_DA_MEDIUM with refDist=100, refBright=0.5:
  // k0 = 1, k1 = 0.5*(1-b)/(b*d), k2 = 0.5*(1-b)/(b*d*d)
  EXPECT_FLOAT_EQ(light.distAtt[0], 1.0f);
  EXPECT_FLOAT_EQ(light.distAtt[1], 0.5f * 0.5f / (0.5f * 100.0f));
  EXPECT_FLOAT_EQ(light.distAtt[2], 0.5f * 0.5f / (0.5f * 100.0f * 100.0f));
}

// --- GXSetChanCtrl (XF 0x100E-0x1011) ---

TEST_F(GXFifoTest, ChanCtrl_Color0_LightingEnabled) {
  GXSetChanCtrl(GX_COLOR0, true, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0 | GX_LIGHT1, GX_DF_CLAMP, GX_AF_SPOT);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& cfg = g_gxState.colorChannelConfig[GX_COLOR0];
  EXPECT_TRUE(cfg.lightingEnabled);
  EXPECT_EQ(cfg.matSrc, GX_SRC_VTX);
  EXPECT_EQ(cfg.ambSrc, GX_SRC_REG);
  EXPECT_EQ(cfg.diffFn, GX_DF_CLAMP);
  EXPECT_EQ(cfg.attnFn, GX_AF_SPOT);

  // Light mask should be 0x03 (lights 0 and 1)
  auto& state = g_gxState.colorChannelState[GX_COLOR0];
  EXPECT_TRUE(state.lightMask[0]);
  EXPECT_TRUE(state.lightMask[1]);
  EXPECT_FALSE(state.lightMask[2]);
}

TEST_F(GXFifoTest, ChanCtrl_Alpha0_NoLighting) {
  GXSetChanCtrl(GX_ALPHA0, false, GX_SRC_VTX, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& cfg = g_gxState.colorChannelConfig[GX_ALPHA0];
  EXPECT_FALSE(cfg.lightingEnabled);
  EXPECT_EQ(cfg.matSrc, GX_SRC_REG);
  EXPECT_EQ(cfg.ambSrc, GX_SRC_VTX);
  EXPECT_EQ(cfg.attnFn, GX_AF_NONE);
}

TEST_F(GXFifoTest, ChanCtrl_Color1_SpecularLighting) {
  GXSetChanCtrl(GX_COLOR1, true, GX_SRC_REG, GX_SRC_REG, GX_LIGHT2 | GX_LIGHT5, GX_DF_SIGN, GX_AF_SPEC);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& cfg = g_gxState.colorChannelConfig[GX_COLOR1];
  EXPECT_TRUE(cfg.lightingEnabled);
  EXPECT_EQ(cfg.matSrc, GX_SRC_REG);
  EXPECT_EQ(cfg.ambSrc, GX_SRC_REG);
  EXPECT_EQ(cfg.diffFn, GX_DF_NONE);
  EXPECT_EQ(cfg.attnFn, GX_AF_SPEC);

  auto& state = g_gxState.colorChannelState[GX_COLOR1];
  EXPECT_FALSE(state.lightMask[0]);
  EXPECT_FALSE(state.lightMask[1]);
  EXPECT_TRUE(state.lightMask[2]);
  EXPECT_FALSE(state.lightMask[3]);
  EXPECT_FALSE(state.lightMask[4]);
  EXPECT_TRUE(state.lightMask[5]);
}

TEST_F(GXFifoTest, ChanCtrl_EncodesHardwareAttenuationField) {
  GXSetChanCtrl(GX_COLOR0, true, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_SIGN, GX_AF_SPEC);
  auto specBytes = capture_fifo();
  ASSERT_GE(specBytes.size(), 9u);
  EXPECT_EQ((read_be32_at(specBytes, 5) >> 7) & 0x3u, static_cast<u32>(GX_DF_NONE));
  EXPECT_EQ((read_be32_at(specBytes, 5) >> 9) & 0x3u, 1u);

  GXSetChanCtrl(GX_COLOR0, true, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
  auto spotBytes = capture_fifo();
  ASSERT_GE(spotBytes.size(), 9u);
  EXPECT_EQ((read_be32_at(spotBytes, 5) >> 9) & 0x3u, 3u);

  GXSetChanCtrl(GX_COLOR0, true, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_NONE, GX_AF_NONE);
  auto noneBytes = capture_fifo();
  ASSERT_GE(noneBytes.size(), 9u);
  EXPECT_EQ((read_be32_at(noneBytes, 5) >> 9) & 0x3u, 2u);
}

TEST_F(GXFifoTest, ChanCtrl_DecodesRawHardwareAttenuationField) {
  reset_gx_state();
  decode_fifo(xf_cmd(0x100E, {1u << 9}));
  EXPECT_EQ(g_gxState.colorChannelConfig[GX_COLOR0].attnFn, GX_AF_SPEC);

  reset_gx_state();
  decode_fifo(xf_cmd(0x100E, {2u << 9}));
  EXPECT_EQ(g_gxState.colorChannelConfig[GX_COLOR0].attnFn, GX_AF_NONE);

  reset_gx_state();
  decode_fifo(xf_cmd(0x100E, {3u << 9}));
  EXPECT_EQ(g_gxState.colorChannelConfig[GX_COLOR0].attnFn, GX_AF_SPOT);

  reset_gx_state();
  decode_fifo(xf_cmd(0x100E, {0u << 9}));
  EXPECT_EQ(g_gxState.colorChannelConfig[GX_COLOR0].attnFn, GX_AF_NONE);
}

TEST_F(GXFifoTest, ChanCtrl_Color0A0_Compound) {
  // GX_COLOR0A0 should set both GX_COLOR0 and GX_ALPHA0
  GXSetChanCtrl(GX_COLOR0A0, true, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Both COLOR0 and ALPHA0 should be configured identically
  auto& cfgC = g_gxState.colorChannelConfig[GX_COLOR0];
  EXPECT_TRUE(cfgC.lightingEnabled);
  EXPECT_EQ(cfgC.matSrc, GX_SRC_VTX);
  EXPECT_EQ(cfgC.ambSrc, GX_SRC_REG);
  EXPECT_EQ(cfgC.diffFn, GX_DF_CLAMP);
  EXPECT_EQ(cfgC.attnFn, GX_AF_SPOT);

  auto& cfgA = g_gxState.colorChannelConfig[GX_ALPHA0];
  EXPECT_TRUE(cfgA.lightingEnabled);
  EXPECT_EQ(cfgA.matSrc, GX_SRC_VTX);
  EXPECT_EQ(cfgA.ambSrc, GX_SRC_REG);
  EXPECT_EQ(cfgA.attnFn, GX_AF_SPOT);

  EXPECT_TRUE(g_gxState.colorChannelState[GX_COLOR0].lightMask[0]);
  EXPECT_TRUE(g_gxState.colorChannelState[GX_ALPHA0].lightMask[0]);
}

TEST_F(GXFifoTest, ChanCtrl_Color1A1_Compound) {
  GXSetChanCtrl(GX_COLOR1A1, false, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& cfgC = g_gxState.colorChannelConfig[GX_COLOR1];
  EXPECT_FALSE(cfgC.lightingEnabled);
  EXPECT_EQ(cfgC.ambSrc, GX_SRC_VTX);
  EXPECT_EQ(cfgC.matSrc, GX_SRC_VTX);

  auto& cfgA = g_gxState.colorChannelConfig[GX_ALPHA1];
  EXPECT_FALSE(cfgA.lightingEnabled);
  EXPECT_EQ(cfgA.ambSrc, GX_SRC_VTX);
  EXPECT_EQ(cfgA.matSrc, GX_SRC_VTX);
}

// --- GXSetTexCoordGen2 (XF 0x1040-0x105F) ---

TEST_F(GXFifoTest, TexCoordGen_Mtx2x4_Tex0) {
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD0];
  EXPECT_EQ(tcg.type, GX_TG_MTX2x4);
  EXPECT_EQ(tcg.src, GX_TG_TEX0);
  EXPECT_EQ(tcg.mtx, GX_TEXMTX0);
  EXPECT_EQ(tcg.postMtx, GX_PTIDENTITY);
}

TEST_F(GXFifoTest, TexCoordGen_Mtx3x4_Nrm) {
  GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0, GX_TRUE, GX_PTTEXMTX0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD1];
  EXPECT_EQ(tcg.type, GX_TG_MTX3x4);
  EXPECT_EQ(tcg.src, GX_TG_NRM);
  EXPECT_EQ(tcg.mtx, GX_TEXMTX0);
  EXPECT_TRUE(tcg.normalize);
  EXPECT_EQ(tcg.postMtx, GX_PTTEXMTX0);
}

TEST_F(GXFifoTest, TexCoordGen_SRTG_Color0) {
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_SRTG, GX_TG_COLOR0, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD0];
  EXPECT_EQ(tcg.type, GX_TG_SRTG);
  EXPECT_EQ(tcg.mtx, GX_TEXMTX0);
}

TEST_F(GXFifoTest, TexCoordGen_NonZeroMtx) {
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, GX_TEXMTX3, GX_FALSE, GX_PTTEXMTX5);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD0];
  EXPECT_EQ(tcg.type, GX_TG_MTX3x4);
  EXPECT_EQ(tcg.src, GX_TG_TEX0);
  EXPECT_EQ(tcg.mtx, GX_TEXMTX3);
  EXPECT_EQ(tcg.postMtx, GX_PTTEXMTX5);
}

TEST_F(GXFifoTest, TexCoordGen_MultipleCoords) {
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, GX_TEXMTX0, GX_FALSE, GX_PTTEXMTX0);
  GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX1, GX_TEXMTX1, GX_FALSE, GX_PTTEXMTX1);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tcgs[0].mtx, GX_TEXMTX0);
  EXPECT_EQ(g_gxState.tcgs[0].postMtx, GX_PTTEXMTX0);
  EXPECT_EQ(g_gxState.tcgs[0].src, GX_TG_TEX0);
  EXPECT_EQ(g_gxState.tcgs[1].mtx, GX_TEXMTX1);
  EXPECT_EQ(g_gxState.tcgs[1].postMtx, GX_PTTEXMTX1);
  EXPECT_EQ(g_gxState.tcgs[1].src, GX_TG_TEX1);
}

TEST_F(GXFifoTest, TexCoordGen_HighCoord_MatIdxB) {
  // TexCoord4+ uses matIdxB
  GXSetTexCoordGen2(GX_TEXCOORD4, GX_TG_MTX2x4, GX_TG_TEX4, GX_TEXMTX5, GX_FALSE, GX_PTTEXMTX3);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD4];
  EXPECT_EQ(tcg.type, GX_TG_MTX2x4);
  EXPECT_EQ(tcg.src, GX_TG_TEX4);
  EXPECT_EQ(tcg.mtx, GX_TEXMTX5);
  EXPECT_EQ(tcg.postMtx, GX_PTTEXMTX3);
}

TEST_F(GXFifoTest, TexCoordGen_Identity) {
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& tcg = g_gxState.tcgs[GX_TEXCOORD0];
  EXPECT_EQ(tcg.mtx, GX_IDENTITY);
  EXPECT_EQ(tcg.postMtx, GX_PTIDENTITY);
}

TEST_F(GXFifoTest, MatrixIndexA_DecodesTexMatricesFromCpPacket) {
  const u32 value = (GX_PNMTX3 << 0) | (GX_TEXMTX3 << 6) | (GX_TEXMTX4 << 12) | (GX_IDENTITY << 18) |
                    (GX_TEXMTX7 << 24);
  auto bytes = cp_cmd(0x30, value);

  reset_gx_state();
  g_gxState.tcgs[0].mtx = GX_IDENTITY;
  g_gxState.tcgs[1].mtx = GX_IDENTITY;
  g_gxState.tcgs[2].mtx = GX_TEXMTX0;
  g_gxState.tcgs[3].mtx = GX_IDENTITY;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.currentPnMtx, 3u);
  EXPECT_EQ(g_gxState.tcgs[0].mtx, GX_TEXMTX3);
  EXPECT_EQ(g_gxState.tcgs[1].mtx, GX_TEXMTX4);
  EXPECT_EQ(g_gxState.tcgs[2].mtx, GX_IDENTITY);
  EXPECT_EQ(g_gxState.tcgs[3].mtx, GX_TEXMTX7);
}

TEST_F(GXFifoTest, MatrixIndexB_DecodesTexMatricesFromCpPacket) {
  const u32 value = (GX_TEXMTX4 << 0) | (GX_TEXMTX5 << 6) | (GX_IDENTITY << 12) | (GX_TEXMTX9 << 18);
  auto bytes = cp_cmd(0x40, value);

  reset_gx_state();
  g_gxState.tcgs[4].mtx = GX_IDENTITY;
  g_gxState.tcgs[5].mtx = GX_IDENTITY;
  g_gxState.tcgs[6].mtx = GX_TEXMTX0;
  g_gxState.tcgs[7].mtx = GX_IDENTITY;
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.tcgs[4].mtx, GX_TEXMTX4);
  EXPECT_EQ(g_gxState.tcgs[5].mtx, GX_TEXMTX5);
  EXPECT_EQ(g_gxState.tcgs[6].mtx, GX_IDENTITY);
  EXPECT_EQ(g_gxState.tcgs[7].mtx, GX_TEXMTX9);
}

// --- GXSetChanAmbColor / GXSetChanMatColor (XF 0x100A-0x100D) ---

TEST_F(GXFifoTest, ChanAmbColor_Color0) {
  GXColor amb = {64, 128, 192, 255};
  GXSetChanAmbColor(GX_COLOR0, amb);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& state = g_gxState.colorChannelState[GX_COLOR0];
  EXPECT_NEAR(state.ambColor[0], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[1], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[2], 192.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[3], 255.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, ChanMatColor_Color0) {
  GXColor mat = {255, 0, 128, 64};
  GXSetChanMatColor(GX_COLOR0, mat);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& state = g_gxState.colorChannelState[GX_COLOR0];
  EXPECT_NEAR(state.matColor[0], 255.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[1], 0.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[2], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[3], 64.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, ChanAmbColor_Color1) {
  GXColor amb = {10, 20, 30, 40};
  GXSetChanAmbColor(GX_COLOR1, amb);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& state = g_gxState.colorChannelState[GX_COLOR1];
  EXPECT_NEAR(state.ambColor[0], 10.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[1], 20.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[2], 30.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.ambColor[3], 40.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, ChanMatColor_Color1) {
  GXColor mat = {100, 150, 200, 250};
  GXSetChanMatColor(GX_COLOR1, mat);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& state = g_gxState.colorChannelState[GX_COLOR1];
  EXPECT_NEAR(state.matColor[0], 100.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[1], 150.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[2], 200.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(state.matColor[3], 250.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, ChanAmbColor_Color0A0_Compound) {
  // GX_COLOR0A0 should write to both COLOR0 and ALPHA0 XF registers
  GXColor amb = {80, 160, 240, 128};
  GXSetChanAmbColor(GX_COLOR0A0, amb);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& stateC = g_gxState.colorChannelState[GX_COLOR0];
  EXPECT_NEAR(stateC.ambColor[0], 80.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.ambColor[1], 160.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.ambColor[2], 240.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.ambColor[3], 128.f / 255.f, 1.f / 255.f);
  // ALPHA0 shares the same XF register as COLOR0, so should match
  auto& stateA = g_gxState.colorChannelState[GX_ALPHA0];
  EXPECT_NEAR(stateA.ambColor[0], 80.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateA.ambColor[3], 128.f / 255.f, 1.f / 255.f);
}

TEST_F(GXFifoTest, ChanMatColor_Color1A1_Compound) {
  GXColor mat = {32, 64, 96, 128};
  GXSetChanMatColor(GX_COLOR1A1, mat);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& stateC = g_gxState.colorChannelState[GX_COLOR1];
  EXPECT_NEAR(stateC.matColor[0], 32.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.matColor[1], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.matColor[2], 96.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateC.matColor[3], 128.f / 255.f, 1.f / 255.f);

  auto& stateA = g_gxState.colorChannelState[GX_ALPHA1];
  EXPECT_NEAR(stateA.matColor[0], 32.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(stateA.matColor[3], 128.f / 255.f, 1.f / 255.f);
}

// GXSetFog (BP 0xEE-0xF2): fog A/B/C parameters, type and color

// --- Fog with perspective linear fog, typical parameters ---
TEST_F(GXFifoTest, Fog_PerspLin_Typical) {
  GXColor fogColor = {128, 200, 255, 255};
  GXSetFog(GX_FOG_PERSP_LIN, 100.f, 900.f, 0.1f, 1000.f, fogColor);
  auto bytes = capture_fifo();

  // Should produce 5 BP writes (0xEE-0xF2): 5 * 5 = 25 bytes
  ASSERT_EQ(bytes.size(), 25u);
  // Verify BP opcodes and register IDs
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0xEE);
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0xEF);
  EXPECT_EQ(bytes[10], 0x61);
  EXPECT_EQ(bytes[11], 0xF0);
  EXPECT_EQ(bytes[15], 0x61);
  EXPECT_EQ(bytes[16], 0xF1);
  EXPECT_EQ(bytes[20], 0x61);
  EXPECT_EQ(bytes[21], 0xF2);

  reset_gx_state();
  decode_fifo(bytes);

  // Compute expected A, B, C from the SDK formula
  float nearZ = 0.1f, farZ = 1000.f, startZ = 100.f, endZ = 900.f;
  float A = (farZ * nearZ) / ((farZ - nearZ) * (endZ - startZ));
  float B = farZ / (farZ - nearZ);
  float C = startZ / (endZ - startZ);

  // Allow tolerance for encoding precision loss (11-bit mantissa)
  EXPECT_NEAR(g_gxState.fog.a, A, std::abs(A) * 1e-3f);
  EXPECT_NEAR(g_gxState.fog.b, B, std::abs(B) * 1e-3f);
  EXPECT_NEAR(g_gxState.fog.c, C, std::abs(C) * 1e-3f);
  expect_fog_raw_fields_match_decoded_state();
  EXPECT_EQ(g_gxState.fog.type, GX_FOG_PERSP_LIN);
  EXPECT_NEAR(g_gxState.fog.color[0], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[1], 200.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[2], 255.f / 255.f, 1.f / 255.f);
}

// --- Orthographic fog keeps the projection bit and uses the SDK ortho coefficients ---
TEST_F(GXFifoTest, Fog_OrthoLin_Typical) {
  GXColor fogColor = {128, 200, 255, 255};
  GXSetFog(GX_FOG_ORTHO_LIN, 100.f, 900.f, 0.1f, 1000.f, fogColor);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 25u);
  const u32 fog3 = (static_cast<u32>(bytes[16]) << 24) | (static_cast<u32>(bytes[17]) << 16) |
                   (static_cast<u32>(bytes[18]) << 8) | static_cast<u32>(bytes[19]);
  EXPECT_EQ((fog3 >> 20) & 1u, 1u);
  EXPECT_EQ((fog3 >> 21) & 7u, static_cast<u32>(GX_FOG_LIN));

  reset_gx_state();
  decode_fifo(bytes);

  float nearZ = 0.1f, farZ = 1000.f, startZ = 100.f, endZ = 900.f;
  float A = (farZ - nearZ) / (endZ - startZ);
  float C = (startZ - nearZ) / (endZ - startZ);

  EXPECT_NEAR(g_gxState.fog.a, A, std::abs(A) * 1e-3f);
  EXPECT_FLOAT_EQ(g_gxState.fog.b, 0.f);
  EXPECT_NEAR(g_gxState.fog.c, C, std::abs(C) * 1e-3f);
  expect_fog_raw_fields_match_decoded_state();
  EXPECT_EQ(g_gxState.fog.type, GX_FOG_ORTHO_LIN);
}

// --- Fog with degenerate parameters (nearZ == farZ) ---
TEST_F(GXFifoTest, Fog_Degenerate_EqualDepths) {
  GXColor fogColor = {0, 0, 0, 255};
  GXSetFog(GX_FOG_PERSP_EXP, 0.f, 100.f, 10.f, 10.f, fogColor);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // When nearZ == farZ, SDK sets A=0, B=0.5, C=0
  EXPECT_FLOAT_EQ(g_gxState.fog.a, 0.f);
  EXPECT_NEAR(g_gxState.fog.b, 0.5f, 1e-3f);
  EXPECT_FLOAT_EQ(g_gxState.fog.c, 0.f);
  expect_fog_raw_fields_match_decoded_state();
  EXPECT_EQ(g_gxState.fog.type, GX_FOG_PERSP_EXP);
}

// --- Fog type: none ---
TEST_F(GXFifoTest, Fog_None) {
  GXColor fogColor = {64, 64, 64, 255};
  GXSetFog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0.f, fogColor);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.fog.type, GX_FOG_NONE);
  EXPECT_FLOAT_EQ(g_gxState.fog.a, 0.f);
  EXPECT_NEAR(g_gxState.fog.b, 0.5f, 1e-3f);
  EXPECT_FLOAT_EQ(g_gxState.fog.c, 0.f);
  expect_fog_raw_fields_match_decoded_state();
  EXPECT_NEAR(g_gxState.fog.color[0], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[1], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[2], 64.f / 255.f, 1.f / 255.f);
}

// --- Fog with perspective reverse exponential squared type ---
TEST_F(GXFifoTest, Fog_PerspRevExp2) {
  GXColor fogColor = {255, 0, 0, 255};
  GXSetFog(GX_FOG_PERSP_REVEXP2, 50.f, 500.f, 1.f, 1000.f, fogColor);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  float nearZ = 1.f, farZ = 1000.f, startZ = 50.f, endZ = 500.f;
  float A = (farZ * nearZ) / ((farZ - nearZ) * (endZ - startZ));
  float B = farZ / (farZ - nearZ);
  float C = startZ / (endZ - startZ);

  EXPECT_NEAR(g_gxState.fog.a, A, std::abs(A) * 1e-3f);
  EXPECT_NEAR(g_gxState.fog.b, B, std::abs(B) * 1e-3f);
  EXPECT_NEAR(g_gxState.fog.c, C, std::abs(C) * 1e-3f);
  expect_fog_raw_fields_match_decoded_state();
  EXPECT_EQ(g_gxState.fog.type, GX_FOG_PERSP_REVEXP2);
  EXPECT_NEAR(g_gxState.fog.color[0], 1.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[1], 0.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.fog.color[2], 0.f, 1.f / 255.f);
}

// GXSetIndTexMtx (BP 0x06-0x0E): indirect texture matrix parameters

// --- IndTexMtx 0 with half-scale diagonal matrix ---
// Note: 11-bit signed range limits values to [-1.0, 0.999], so 1.0 is not representable.
TEST_F(GXFifoTest, IndTexMtx0_HalfScale) {
  f32 mtx[2][3] = {
      {0.5f, 0.0f, 0.0f},
      {0.0f, 0.5f, 0.0f},
  };
  GXSetIndTexMtx(GX_ITM_0, mtx, 0);
  auto bytes = capture_fifo();

  // Should produce 3 BP writes: 3 * 5 = 15 bytes
  ASSERT_EQ(bytes.size(), 15u);
  // Verify BP opcodes and register IDs (0x06, 0x07, 0x08 for matrix 0)
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x06);
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0x07);
  EXPECT_EQ(bytes[10], 0x61);
  EXPECT_EQ(bytes[11], 0x08);

  reset_gx_state();
  decode_fifo(bytes);

  const auto& info = g_gxState.indTexMtxs[0];
  // 11-bit fixed-point (1/1024) precision
  float tol = 1.0f / 1024.0f;
  EXPECT_NEAR(info.mtx.m0.x, 0.5f, tol);
  EXPECT_NEAR(info.mtx.m0.y, 0.0f, tol);
  EXPECT_NEAR(info.mtx.m1.x, 0.0f, tol);
  EXPECT_NEAR(info.mtx.m1.y, 0.5f, tol);
  EXPECT_NEAR(info.mtx.m2.x, 0.0f, tol);
  EXPECT_NEAR(info.mtx.m2.y, 0.0f, tol);
  EXPECT_EQ(info.scaleExp, 0);
}

// --- IndTexMtx 1 with fractional values and positive scale ---
TEST_F(GXFifoTest, IndTexMtx1_FractionalWithScale) {
  f32 mtx[2][3] = {
      {0.5f, 0.25f, -0.125f},
      {-0.5f, 0.75f, 0.0f},
  };
  GXSetIndTexMtx(GX_ITM_1, mtx, 3);
  auto bytes = capture_fifo();

  // Register IDs for matrix 1: 0x09, 0x0A, 0x0B
  ASSERT_EQ(bytes.size(), 15u);
  EXPECT_EQ(bytes[1], 0x09);
  EXPECT_EQ(bytes[6], 0x0A);
  EXPECT_EQ(bytes[11], 0x0B);

  reset_gx_state();
  decode_fifo(bytes);

  const auto& info = g_gxState.indTexMtxs[1];
  float tol = 1.0f / 1024.0f;
  EXPECT_NEAR(info.mtx.m0.x, 0.5f, tol);
  EXPECT_NEAR(info.mtx.m0.y, -0.5f, tol);
  EXPECT_NEAR(info.mtx.m1.x, 0.25f, tol);
  EXPECT_NEAR(info.mtx.m1.y, 0.75f, tol);
  EXPECT_NEAR(info.mtx.m2.x, -0.125f, tol);
  EXPECT_NEAR(info.mtx.m2.y, 0.0f, tol);
  EXPECT_EQ(info.scaleExp, 3);
}

// --- IndTexMtx 2 with negative scale exponent ---
TEST_F(GXFifoTest, IndTexMtx2_NegativeScale) {
  f32 mtx[2][3] = {
      {0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 0.0f},
  };
  GXSetIndTexMtx(GX_ITM_2, mtx, -5);
  auto bytes = capture_fifo();

  // Register IDs for matrix 2: 0x0C, 0x0D, 0x0E
  ASSERT_EQ(bytes.size(), 15u);
  EXPECT_EQ(bytes[1], 0x0C);
  EXPECT_EQ(bytes[6], 0x0D);
  EXPECT_EQ(bytes[11], 0x0E);

  reset_gx_state();
  decode_fifo(bytes);

  const auto& info = g_gxState.indTexMtxs[2];
  EXPECT_EQ(info.scaleExp, -5);
}

TEST_F(GXFifoTest, IndTexMtxScaleMultiplier_MatchesHardwareExponent) {
  EXPECT_FLOAT_EQ(aurora::gx::indirect_matrix_scale_multiplier(0), 1.0f);
  EXPECT_FLOAT_EQ(aurora::gx::indirect_matrix_scale_multiplier(3), 8.0f);
  EXPECT_FLOAT_EQ(aurora::gx::indirect_matrix_scale_multiplier(-5), 0.03125f);
}

TEST_F(GXFifoTest, IndTexMtxDynamicAliases_MapToSameSlotsAsSdk) {
  f32 mtxS[2][3] = {
      {0.25f, 0.0f, 0.0f},
      {0.0f, 0.25f, 0.0f},
  };
  f32 mtxT[2][3] = {
      {0.5f, 0.0f, 0.0f},
      {0.0f, 0.5f, 0.0f},
  };

  GXSetIndTexMtx(GX_ITM_S1, mtxS, 2);
  GXSetIndTexMtx(GX_ITM_T2, mtxT, -3);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 30u);
  EXPECT_EQ(bytes[1], 0x09);
  EXPECT_EQ(bytes[6], 0x0A);
  EXPECT_EQ(bytes[11], 0x0B);
  EXPECT_EQ(bytes[16], 0x0C);
  EXPECT_EQ(bytes[21], 0x0D);
  EXPECT_EQ(bytes[26], 0x0E);

  reset_gx_state();
  decode_fifo(bytes);

  constexpr float tol = 1.0f / 1024.0f;
  EXPECT_NEAR(g_gxState.indTexMtxs[1].mtx.m0.x, 0.25f, tol);
  EXPECT_NEAR(g_gxState.indTexMtxs[1].mtx.m1.y, 0.25f, tol);
  EXPECT_EQ(g_gxState.indTexMtxs[1].scaleExp, 2);
  EXPECT_NEAR(g_gxState.indTexMtxs[2].mtx.m0.x, 0.5f, tol);
  EXPECT_NEAR(g_gxState.indTexMtxs[2].mtx.m1.y, 0.5f, tol);
  EXPECT_EQ(g_gxState.indTexMtxs[2].scaleExp, -3);
}

// --- IndTexMtx 0 does not affect matrix 1 ---
TEST_F(GXFifoTest, IndTexMtx0_Isolation) {
  f32 mtx0[2][3] = {
      {0.5f, 0.0f, 0.0f},
      {0.0f, 0.5f, 0.0f},
  };
  f32 mtx1[2][3] = {
      {-1.0f, 0.0f, 0.0f},
      {0.0f, -1.0f, 0.0f},
  };
  GXSetIndTexMtx(GX_ITM_0, mtx0, 1);
  GXSetIndTexMtx(GX_ITM_1, mtx1, -2);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  float tol = 1.0f / 1024.0f;
  // Matrix 0
  EXPECT_NEAR(g_gxState.indTexMtxs[0].mtx.m0.x, 0.5f, tol);
  EXPECT_NEAR(g_gxState.indTexMtxs[0].mtx.m1.y, 0.5f, tol);
  EXPECT_EQ(g_gxState.indTexMtxs[0].scaleExp, 1);
  // Matrix 1
  EXPECT_NEAR(g_gxState.indTexMtxs[1].mtx.m0.x, -1.0f, tol);
  EXPECT_NEAR(g_gxState.indTexMtxs[1].mtx.m1.y, -1.0f, tol);
  EXPECT_EQ(g_gxState.indTexMtxs[1].scaleExp, -2);
}

// SU texture coordinate scale (BP 0x30-0x3F)

// --- GXSetTexCoordScaleManually sets width/height ---
TEST_F(GXFifoTest, TexCoordScale_Manual_Coord0) {
  GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, 256, 128);
  auto bytes = capture_fifo();

  // Two BP writes (suTs0 + suTs1): 2 * 5 = 10 bytes
  ASSERT_EQ(bytes.size(), 10u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x30); // suTs0[0]
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0x31); // suTs1[0]

  reset_gx_state();
  decode_fifo(bytes);

  const auto& tcs = g_gxState.texCoordScales[0];
  EXPECT_EQ(tcs.scaleS, 255u); // width - 1
  EXPECT_EQ(tcs.scaleT, 127u); // height - 1
}

// --- GXSetTexCoordScaleManually for coord 3 ---
TEST_F(GXFifoTest, TexCoordScale_Manual_Coord3) {
  GXSetTexCoordScaleManually(GX_TEXCOORD3, GX_TRUE, 512, 512);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 10u);
  EXPECT_EQ(bytes[1], 0x36); // suTs0[3] = 0x30 + 3*2
  EXPECT_EQ(bytes[6], 0x37); // suTs1[3] = 0x31 + 3*2

  reset_gx_state();
  decode_fifo(bytes);

  const auto& tcs = g_gxState.texCoordScales[3];
  EXPECT_EQ(tcs.scaleS, 511u);
  EXPECT_EQ(tcs.scaleT, 511u);
}

// --- GXSetTexCoordScaleManually with bias and cyl wrap ---
TEST_F(GXFifoTest, TexCoordScale_BiasAndCylWrap) {
  // Enable manual mode first, then set bias and cyl wrap
  GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, 64, 64);
  capture_fifo(); // discard

  GXSetTexCoordBias(GX_TEXCOORD0, GX_TRUE, GX_FALSE);
  auto biasBytes = capture_fifo();

  GXSetTexCoordCylWrap(GX_TEXCOORD0, GX_FALSE, GX_TRUE);
  auto cylBytes = capture_fifo();

  // Each writes 2 BP regs
  ASSERT_EQ(biasBytes.size(), 10u);
  ASSERT_EQ(cylBytes.size(), 10u);

  reset_gx_state();
  decode_fifo(biasBytes);
  decode_fifo(cylBytes);

  const auto& tcs = g_gxState.texCoordScales[0];
  EXPECT_TRUE(tcs.biasS);
  EXPECT_FALSE(tcs.biasT);
  EXPECT_FALSE(tcs.cylWrapS);
  EXPECT_TRUE(tcs.cylWrapT);
}

// --- GXEnableTexOffsets ---
TEST_F(GXFifoTest, TexCoordScale_TexOffsets) {
  GXEnableTexOffsets(GX_TEXCOORD2, GX_TRUE, GX_TRUE);
  auto bytes = capture_fifo();

  // One BP write (suTs0 only): 5 bytes
  ASSERT_EQ(bytes.size(), 5u);
  EXPECT_EQ(bytes[1], 0x34); // suTs0[2] = 0x30 + 2*2

  reset_gx_state();
  decode_fifo(bytes);

  const auto& tcs = g_gxState.texCoordScales[2];
  EXPECT_TRUE(tcs.lineOffset);
  EXPECT_TRUE(tcs.pointOffset);
}

TEST_F(GXFifoTest, TexCoordScale_TexOffsets_Disabled) {
  GXEnableTexOffsets(GX_TEXCOORD2, GX_FALSE, GX_FALSE);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 5u);
  EXPECT_EQ(bytes[1], 0x34);

  reset_gx_state();
  g_gxState.texCoordScales[2].lineOffset = true;
  g_gxState.texCoordScales[2].pointOffset = true;
  decode_fifo(bytes);

  const auto& tcs = g_gxState.texCoordScales[2];
  EXPECT_FALSE(tcs.lineOffset);
  EXPECT_FALSE(tcs.pointOffset);
}

// --- Coord isolation: writing coord 0 doesn't affect coord 1 ---
TEST_F(GXFifoTest, TexCoordScale_Isolation) {
  GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, 100, 200);
  GXSetTexCoordScaleManually(GX_TEXCOORD1, GX_TRUE, 300, 400);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.texCoordScales[0].scaleS, 99u);
  EXPECT_EQ(g_gxState.texCoordScales[0].scaleT, 199u);
  EXPECT_EQ(g_gxState.texCoordScales[1].scaleS, 299u);
  EXPECT_EQ(g_gxState.texCoordScales[1].scaleT, 399u);
}

// GXSetCopyClear (BP 0x4F-0x51): clear color and depth

TEST_F(GXFifoTest, DispCopyState_EncodesBpAndYScale) {
  GXSetDispCopySrc(4, 8, 640, 480);
  GXSetDispCopyDst(640, 480);
  const u32 lines = GXSetDispCopyYScale(1.0f);
  auto bytes = capture_fifo();

  EXPECT_EQ(lines, 480u);
  EXPECT_TRUE(has_bp_write(bytes, 0x49));
  EXPECT_TRUE(has_bp_write(bytes, 0x4A));
  EXPECT_TRUE(has_bp_write(bytes, 0x4D));
  EXPECT_TRUE(has_bp_write(bytes, 0x4E));

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.dispCopySrc.x, 4);
  EXPECT_EQ(g_gxState.dispCopySrc.y, 8);
  EXPECT_EQ(g_gxState.dispCopySrc.width, 640);
  EXPECT_EQ(g_gxState.dispCopySrc.height, 480);
  EXPECT_NEAR(g_gxState.dispCopyYScale, 1.0f, 0.001f);
}

TEST_F(GXFifoTest, DispCopyYScaleHelpers_MatchIdentityScale) {
  EXPECT_EQ(GXGetNumXfbLines(480, 1.0f), 480u);
  EXPECT_NEAR(GXGetYScaleFactor(480, 480), 1.0f, 0.001f);
}

TEST_F(GXFifoTest, CopyFilter_EncodesAndDecodesAaAndVerticalFilter) {
  u8 samples[12][2] = {};
  for (size_t i = 0; i < 12; ++i) {
    samples[i][0] = static_cast<u8>((i * 2) & 0x0f);
    samples[i][1] = static_cast<u8>((i * 2 + 1) & 0x0f);
  }
  u8 vfilter[7] = {1, 2, 3, 4, 5, 6, 7};

  GXSetCopyFilter(GX_TRUE, samples, GX_TRUE, vfilter);
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 30u);
  EXPECT_EQ(bytes[1], 0x01);
  EXPECT_EQ(bytes[6], 0x02);
  EXPECT_EQ(bytes[11], 0x03);
  EXPECT_EQ(bytes[16], 0x04);
  EXPECT_EQ(bytes[21], 0x53);
  EXPECT_EQ(bytes[26], 0x54);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_TRUE(g_gxState.copyFilterAa);
  EXPECT_TRUE(g_gxState.copyFilterVf);
  for (size_t i = 0; i < 12; ++i) {
    EXPECT_EQ(g_gxState.copyFilterSamplePattern[i][0], samples[i][0]);
    EXPECT_EQ(g_gxState.copyFilterSamplePattern[i][1], samples[i][1]);
  }
  for (size_t i = 0; i < 7; ++i) {
    EXPECT_EQ(g_gxState.copyFilterVFilter[i], vfilter[i]);
  }
}

TEST_F(GXFifoTest, CopyFilter_DisabledUsesSdkFallbackCoefficients) {
  u8 samples[12][2] = {};
  u8 vfilter[7] = {};

  GXSetCopyFilter(GX_FALSE, samples, GX_FALSE, vfilter);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_FALSE(g_gxState.copyFilterAa);
  EXPECT_FALSE(g_gxState.copyFilterVf);
  for (const auto& sample : g_gxState.copyFilterSamplePattern) {
    EXPECT_EQ(sample[0], 6u);
    EXPECT_EQ(sample[1], 6u);
  }
  const std::array<u8, 7> expectedVFilter{0, 0, 21, 22, 21, 0, 0};
  EXPECT_EQ(g_gxState.copyFilterVFilter, expectedVFilter);
}

TEST_F(GXFifoTest, DispCopyGamma_UpdatesCopyTriggerShadowBits) {
  GXSetDispCopyGamma(GX_GM_2_2);

  EXPECT_EQ(g_gxState.dispCopyGamma, GX_GM_2_2);
  EXPECT_EQ((g_gxState.bpRegCache[0x52] >> 7) & 3u, static_cast<u32>(GX_GM_2_2));
}

TEST_F(GXFifoTest, CopyTrigger_DecodesClampGammaFormatHalfScaleAndFrameMode) {
  const u32 value = (static_cast<u32>(GX_CLAMP_TOP) << 0) | (static_cast<u32>(GX_TF_RGBA8) << 3) |
                    (static_cast<u32>(GX_GM_1_7) << 7) | (1u << 9) | (2u << 12);
  auto bytes = bp_cmd(0x52, value);

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.copyClamp, GX_CLAMP_TOP);
  EXPECT_EQ(g_gxState.texCopyFmt, GX_TF_RGBA8);
  EXPECT_EQ(g_gxState.dispCopyGamma, GX_GM_1_7);
  EXPECT_TRUE(g_gxState.texCopyHalfScale);
  EXPECT_EQ(g_gxState.dispCopyFrame2Field, 2u);
}

TEST_F(GXFifoTest, CopyTexClearTruePassesScratchRectAndUpdateMasksToResolve) {
  std::array<u8, 152 * 114 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;
  gxState().colorUpdate = true;
  gxState().alphaUpdate = true;
  gxState().depthUpdate = true;
  gxState().dstAlpha = UINT32_MAX;
  gxState().clearColor = {64.f / 255.f, 128.f / 255.f, 192.f / 255.f, 32.f / 255.f};
  gxState().clearDepth = 0x123456;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{336, 300, 152, 114}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_EQ(resolve.sourceRectPixels->x(), 336.f);
  EXPECT_EQ(resolve.sourceRectPixels->y(), 300.f);
  EXPECT_EQ(resolve.sourceRectPixels->z(), 152.f);
  EXPECT_EQ(resolve.sourceRectPixels->w(), 114.f);
  EXPECT_TRUE(resolve.clearColor);
  EXPECT_TRUE(resolve.clearAlpha);
  EXPECT_TRUE(resolve.clearDepth);
  EXPECT_NEAR(resolve.clearColorValue.x(), 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(resolve.clearColorValue.y(), 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(resolve.clearColorValue.z(), 192.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(resolve.clearColorValue.w(), 32.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(resolve.clearDepthValue, 1.f - 0x123456 / 16777216.f, 1.f / 16777216.f);
  EXPECT_EQ(resolve.resolveFormat, GX_TF_RGBA8);
  EXPECT_FALSE(resolve.halfScale);
  EXPECT_FALSE(resolve.forceOpaqueAlpha);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).revision, 1u);
}

TEST_F(GXFifoTest, CopyTexColorFormatMarksResolvePersistent) {
  std::array<u8, 152 * 114 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records.front().persistentCopy);
}

TEST_F(GXFifoTest, RecurringColorCopyKeepsLaterResolveSkippable) {
  std::array<u8, 152 * 114 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  aurora::gfx::testing::set_current_frame(10);
  GXCopyTex(image.data(), GX_FALSE);
  aurora::gfx::testing::set_current_frame(11);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[0].persistentCopy);
  EXPECT_FALSE(records[1].persistentCopy);
}

TEST_F(GXFifoTest, ColorCopyAfterFrameGapRegainsPersistentProtection) {
  std::array<u8, 152 * 114 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  aurora::gfx::testing::set_current_frame(10);
  GXCopyTex(image.data(), GX_FALSE);
  aurora::gfx::testing::set_current_frame(12);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[0].persistentCopy);
  EXPECT_TRUE(records[1].persistentCopy);
}

TEST_F(GXFifoTest, CopyTexDepthFormatKeepsResolveSkippable) {
  std::array<u8, 4 * 4 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(0, 0, 4, 4);
  GXSetTexCopyDst(4, 4, GX_TF_Z24X8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_FALSE(records.front().persistentCopy);
}

TEST_F(GXFifoTest, CopyDispResolveIsNotPersistent) {
  GXSetDispCopySrc(0, 0, 32, 32);
  GXSetDispCopyDst(32, 32);
  GXCopyDisp(nullptr, GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_FALSE(records.front().persistentCopy);
}

TEST_F(GXFifoTest, CopyTexMipmapDstRequestsHalfScaleResolve) {
  std::array<u8, 76 * 57 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(76, 57, GX_TF_RGBA8, GX_TRUE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{336, 300, 152, 114}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_EQ(resolve.sourceRectPixels->x(), 336.f);
  EXPECT_EQ(resolve.sourceRectPixels->y(), 300.f);
  EXPECT_EQ(resolve.sourceRectPixels->z(), 152.f);
  EXPECT_EQ(resolve.sourceRectPixels->w(), 114.f);
  EXPECT_TRUE(resolve.halfScale);
  EXPECT_TRUE(gxState().texCopyHalfScale);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).width, 76u);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).height, 57u);
}

TEST_F(GXFifoTest, CopyTexRetainsInternalResolutionInGpuCopyAndGuestDimensionsInCache) {
  std::array<u8, 152 * 114 * 4> image{};
  aurora::gfx::testing::set_framebuffer_sizes(640, 528, 2560, 2112);
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{1344, 1200, 608, 456}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_EQ(resolve.sourceRectPixels->x(), 1344.f);
  EXPECT_EQ(resolve.sourceRectPixels->y(), 1200.f);
  EXPECT_EQ(resolve.sourceRectPixels->z(), 608.f);
  EXPECT_EQ(resolve.sourceRectPixels->w(), 456.f);
  ASSERT_TRUE(resolve.texture);
  EXPECT_EQ(resolve.texture->size.width, 608u);
  EXPECT_EQ(resolve.texture->size.height, 456u);
  EXPECT_FLOAT_EQ(resolve.copyFilterRowStride, 4.0f);

  const auto& copy = gxState().copyTextures.at(image.data());
  EXPECT_EQ(copy.width, 152u);
  EXPECT_EQ(copy.height, 114u);
  EXPECT_EQ(copy.dataSize, GXGetTexBufferSize(152, 114, GX_TF_RGBA8, GX_FALSE, 0));
}

TEST_F(GXFifoTest, CopyTexUsesExactRationalScaledEdgesBeforeClearing) {
  std::array<u8, 32 * 32 * 4> image{};
  // float32 evaluates 7 * (62 / 14) just below 31. Without stabilizing
  // the rational copy edges, floor() expands this clear to begin at x/y 30.
  aurora::gfx::testing::set_framebuffer_sizes(14, 14, 62, 62);
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(7, 7, 7, 7);
  GXSetTexCopyDst(31, 31, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{31, 31, 31, 31}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->x(), 31.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->y(), 31.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->z(), 31.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->w(), 31.0f);
}

TEST_F(GXFifoTest, CopyTexClearDoesNotOverlapFractionalLeftEdge) {
  std::array<u8, 152 * 114 * 4> image{};
  // MKW split-screen scratch-copy geometry from the regression capture: the scaled source edge is
  // x=492.975, so flooring clears column 492 while the matching viewport rasterizes from 493.
  aurora::gfx::testing::set_framebuffer_sizes(640, 528, 939, 528);
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{493, 300, 223, 114}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->x(), 492.975f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->y(), 300.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->z(), 223.0125f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->w(), 114.0f);
}

TEST_F(GXFifoTest, CopyTexClearBoundsRemainExactAtSixTimesInternalResolution) {
  std::array<u8, 152 * 114 * 4> image{};
  aurora::gfx::testing::set_framebuffer_sizes(640, 528, 3840, 3168);
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(336, 300, 152, 114);
  GXSetTexCopyDst(152, 114, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  const auto& resolve = records.front();
  EXPECT_EQ(resolve.rect, (aurora::gfx::ClipRect{2016, 1800, 912, 684}));
  ASSERT_TRUE(resolve.sourceRectPixels.has_value());
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->x(), 2016.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->y(), 1800.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->z(), 912.0f);
  EXPECT_FLOAT_EQ(resolve.sourceRectPixels->w(), 684.0f);
}

TEST_F(GXFifoTest, CopyTexRecreatesGpuCopyWhenInternalResolutionChanges) {
  std::array<u8, 64 * 64 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;
  GXSetTexCopySrc(0, 0, 64, 64);
  GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);

  GXCopyTex(image.data(), GX_FALSE);
  const auto* nativeHandle = gxState().copyTextures.at(image.data()).handle.get();
  ASSERT_NE(nativeHandle, nullptr);
  EXPECT_EQ(nativeHandle->size.width, 64u);

  aurora::gfx::testing::set_framebuffer_sizes(640, 480, 1280, 960);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& scaledCopy = gxState().copyTextures.at(image.data());
  ASSERT_TRUE(scaledCopy.handle);
  EXPECT_NE(scaledCopy.handle.get(), nativeHandle);
  EXPECT_EQ(scaledCopy.handle->size.width, 128u);
  EXPECT_EQ(scaledCopy.handle->size.height, 128u);
  EXPECT_EQ(scaledCopy.width, 64u);
  EXPECT_EQ(scaledCopy.height, 64u);
  EXPECT_EQ(scaledCopy.revision, 2u);
}

TEST_F(GXFifoTest, CopyTexPassesVerticalCopyFilterCoefficientsToResolve) {
  std::array<u8, 32 * 32 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;
  u8 vfilter[7] = {3, 5, 7, 11, 13, 17, 19};

  GXSetCopyFilter(GX_FALSE, nullptr, GX_TRUE, vfilter);
  GXSetTexCopySrc(0, 0, 32, 32);
  GXSetTexCopyDst(32, 32, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().copyFilterCoefficients, (std::array<u32, 3>{8, 31, 36}));
}

TEST_F(GXFifoTest, CopyTexPassesVerticalCopyClampToResolve) {
  std::array<u8, 32 * 32 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetCopyClamp(GX_CLAMP_TOP);
  GXSetTexCopySrc(8, 12, 32, 32);
  GXSetTexCopyDst(32, 32, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records.front().clampTop);
  EXPECT_FALSE(records.front().clampBottom);
}

TEST_F(GXFifoTest, CopyDispCanDisableSpatialCopyFilterWithoutChangingBrightness) {
  u8 vfilter[7] = {3, 5, 7, 11, 13, 17, 19};
  aurora::g_config.disableCopyFilter = true;

  GXSetCopyFilter(GX_FALSE, nullptr, GX_TRUE, vfilter);
  GXSetDispCopySrc(0, 0, 32, 32);
  GXSetDispCopyDst(32, 32);
  GXCopyDisp(nullptr, GX_FALSE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().copyFilterCoefficients, (std::array<u32, 3>{0, 75, 0}));
}

TEST_F(GXFifoTest, CopyTexDepthZ16PreservesVerticalCopyFilterForConversion) {
  std::array<u8, 32 * 32 * 2> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;
  u8 vfilter[7] = {21, 0, 0, 22, 0, 21, 0};

  GXSetCopyFilter(GX_FALSE, nullptr, GX_TRUE, vfilter);
  EXPECT_TRUE(gxState().copyFilterVf);
  EXPECT_EQ(gxState().copyFilterVFilter, (std::array<u8, 7>{21, 0, 0, 22, 0, 21, 0}));
  GXSetTexCopySrc(0, 0, 32, 32);
  GXSetTexCopyDst(32, 32, GX_TF_Z16, GX_FALSE);
  EXPECT_EQ(gxState().copyFilterVFilter, (std::array<u8, 7>{21, 0, 0, 22, 0, 21, 0}));
  GXCopyTex(image.data(), GX_FALSE);
  EXPECT_EQ(gxState().copyFilterVFilter, (std::array<u8, 7>{21, 0, 0, 22, 0, 21, 0}));

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().resolveFormat, GX_TF_Z16);
  EXPECT_EQ(records.front().copyFilterCoefficients, (std::array<u32, 3>{21, 22, 21}));
  EXPECT_FALSE(records.front().forceOpaqueAlpha);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).format, GX_TF_Z16);
}

TEST_F(GXFifoTest, CopyTexDepthZ16UsesRa8Ia8Semantics) {
  constexpr auto rgba = aurora::gfx::tex_copy_conv::detail::z16_ra8_as_ia8_rgba(0x12);
  EXPECT_EQ(rgba, (std::array<std::uint8_t, 4>{0x12, 0x12, 0x12, 0xff}));

  constexpr auto shader = aurora::gfx::tex_copy_conv::detail::Z16FragmentShader;
  EXPECT_NE(shader.find("let i = f32(depth_bytes.r)"), std::string_view::npos);
  EXPECT_NE(shader.find("return vec4f(i, i, i, 1.0)"), std::string_view::npos);
}

TEST_F(GXFifoTest, CopyTexClearTrueNoOpsWhenAllUpdateMasksAreDisabled) {
  std::array<u8, 64 * 64 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;
  gxState().colorUpdate = false;
  gxState().alphaUpdate = false;
  gxState().depthUpdate = false;
  gxState().dstAlpha = UINT32_MAX;

  GXSetTexCopySrc(8, 16, 64, 64);
  GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_FALSE(records.front().clearColor);
  EXPECT_FALSE(records.front().clearAlpha);
  EXPECT_FALSE(records.front().clearDepth);
}

TEST_F(GXFifoTest, CopyTexClearTrueRgbTargetKeepsFormatAndForcesOpaqueCopiedAlpha) {
  std::array<u8, 32 * 32 * 4> image{};
  gxState().pixelFmt = GX_PF_RGB8_Z24;
  gxState().colorUpdate = true;
  gxState().alphaUpdate = false;
  gxState().depthUpdate = false;
  gxState().dstAlpha = UINT32_MAX;
  gxState().clearColor = {0.2f, 0.4f, 0.6f, 0.25f};

  GXSetTexCopySrc(0, 0, 32, 32);
  GXSetTexCopyDst(32, 32, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_TRUE);

  const auto& records = aurora::gfx::testing::resolve_pass_records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records.front().clearColor);
  EXPECT_FALSE(records.front().clearAlpha);
  EXPECT_FALSE(records.front().clearDepth);
  EXPECT_NEAR(records.front().clearColorValue.x(), 0.2f, 1.f / 255.f);
  EXPECT_NEAR(records.front().clearColorValue.y(), 0.4f, 1.f / 255.f);
  EXPECT_NEAR(records.front().clearColorValue.z(), 0.6f, 1.f / 255.f);
  EXPECT_EQ(records.front().clearColorValue.w(), 0.25f);
  EXPECT_EQ(records.front().resolveFormat, GX_TF_RGBA8);
  EXPECT_TRUE(records.front().forceOpaqueAlpha);
}

TEST_F(GXFifoTest, CopyTexSameTargetReusesHostTextureUntilSampledThisFrame) {
  std::array<u8, 64 * 64 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(0, 0, 64, 64);
  GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const auto& first = gxState().copyTextures.at(image.data());
  ASSERT_TRUE(first.handle);
  const auto* firstHandle = first.handle.get();
  EXPECT_EQ(first.revision, 1u);

  GXCopyTex(image.data(), GX_FALSE);

  const auto& second = gxState().copyTextures.at(image.data());
  EXPECT_EQ(second.handle.get(), firstHandle);
  EXPECT_EQ(second.revision, 2u);
}

TEST_F(GXFifoTest, CopyTexSameFrameSampledTargetGetsFreshHostTexture) {
  std::array<u8, 64 * 64 * 4> image{};
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(0, 0, 64, 64);
  GXSetTexCopyDst(64, 64, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const aurora::gx::GXState::CopyTextureKey key{
      .dest = image.data(),
      .width = 64,
      .height = 64,
      .format = GX_TF_RGBA8,
  };
  const auto& first = gxState().copyTextures.at(image.data());
  ASSERT_TRUE(first.handle);
  const auto* firstHandle = first.handle.get();

  auto& cached = gxState().copyTextureCache.at(key);
  cached.lastSampledFrame = aurora::gfx::current_frame();
  cached.sampledThisFrame = true;

  GXCopyTex(image.data(), GX_FALSE);

  const auto& second = gxState().copyTextures.at(image.data());
  ASSERT_TRUE(second.handle);
  EXPECT_NE(second.handle.get(), firstHandle);
  EXPECT_EQ(second.revision, 2u);
}

TEST_F(GXFifoTest, CopyTexSameTargetDifferentShapeRetiresHistoricalCacheEntry) {
  std::vector<u8> image(608 * 456 * 4);
  gxState().pixelFmt = GX_PF_RGBA6_Z24;

  GXSetTexCopySrc(0, 0, 608, 456);
  GXSetTexCopyDst(608, 456, GX_TF_RGB565, GX_FALSE);
  GXCopyTex(image.data(), GX_FALSE);

  const aurora::gx::GXState::CopyTextureKey fullKey{
      .dest = image.data(),
      .width = 608,
      .height = 456,
      .format = GX_TF_RGB565,
  };
  const auto& fullCopy = gxState().copyTextureCache.at(fullKey);
  ASSERT_TRUE(fullCopy.handle);
  const auto* fullHandle = fullCopy.handle.get();

  GXSetTexCopySrc(0, 0, 304, 228);
  GXSetTexCopyDst(304, 228, GX_TF_RGBA8, GX_TRUE);
  GXCopyTex(image.data(), GX_FALSE);

  const aurora::gx::GXState::CopyTextureKey halfKey{
      .dest = image.data(),
      .width = 304,
      .height = 228,
      .format = GX_TF_RGBA8,
  };
  const auto& halfCopy = gxState().copyTextureCache.at(halfKey);
  ASSERT_TRUE(halfCopy.handle);

  EXPECT_FALSE(gxState().copyTextureCache.contains(fullKey));
  EXPECT_EQ(gxState().copyTextureCache.size(), 1u);
  EXPECT_NE(halfCopy.handle.get(), fullHandle);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).handle.get(), halfCopy.handle.get());
  EXPECT_EQ(gxState().copyTextures.at(image.data()).width, 304u);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).height, 228u);
  EXPECT_EQ(gxState().copyTextures.at(image.data()).format, GX_TF_RGBA8);
}

TEST_F(GXFifoTest, CopyTexCtfFormatsCanBackCompatibleTextureFormats) {
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_R4, GX_TF_I4));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_R8, GX_TF_I8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_G8, GX_TF_I8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_B8, GX_TF_I8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_RA4, GX_TF_IA4));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_RA8, GX_TF_IA8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_RG8, GX_TF_IA8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_GB8, GX_TF_IA8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_TF_Z16, GX_TF_IA8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_CTF_YUVA8, GX_TF_RGBA8));
  EXPECT_TRUE(aurora::gx::copy_texture_format_compatible(GX_TF_Z24X8, GX_TF_RGBA8));
  EXPECT_FALSE(aurora::gx::copy_texture_format_compatible(GX_CTF_R8, GX_TF_RGBA8));
}

TEST_F(GXFifoTest, FieldMaskModeRevBitsAndFogRange_DecodeKnownBpState) {
  std::vector<u8> bytes;
  const auto fieldMask = bp_cmd(0x44, 0x03);
  const auto revBits = bp_cmd(0x58, 0x0F);
  const auto fieldMode = bp_cmd(0x68, 0x01);
  const auto fogRangeBase = bp_cmd(0xE8, 0x0156);
  const auto fogRangeK0 = bp_cmd(0xE9, 0x123456);
  bytes.insert(bytes.end(), fieldMask.begin(), fieldMask.end());
  bytes.insert(bytes.end(), revBits.begin(), revBits.end());
  bytes.insert(bytes.end(), fieldMode.begin(), fieldMode.end());
  bytes.insert(bytes.end(), fogRangeBase.begin(), fogRangeBase.end());
  bytes.insert(bytes.end(), fogRangeK0.begin(), fogRangeK0.end());

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.fieldMask, 0x03u);
  EXPECT_EQ(g_gxState.revBits, 0x0Fu);
  EXPECT_EQ(g_gxState.fieldMode, 0x01u);
  EXPECT_EQ(g_gxState.fogRange[0], 0x0156u);
  EXPECT_EQ(g_gxState.fogRange[1], 0x123456u);
}

TEST_F(GXFifoTest, XfErrorAndDualTex_DecodeKnownRawState) {
  std::vector<u8> bytes;
  const auto xfError = xf_cmd(0x1000, {0x3F});
  const auto dualTex = xf_cmd(0x1012, {0x01});
  bytes.insert(bytes.end(), xfError.begin(), xfError.end());
  bytes.insert(bytes.end(), dualTex.begin(), dualTex.end());

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.xfError, 0x3Fu);
  EXPECT_EQ(g_gxState.dualTex, 0x01u);
}

TEST_F(GXFifoTest, ClearBoundingBox_EncodesBpAndDecodesFallbackExtents) {
  GXClearBoundingBox();
  auto bytes = capture_fifo();

  ASSERT_EQ(bytes.size(), 10u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x55);
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0x56);

  reset_gx_state();
  g_gxState.boundingBox = {1, 2, 3, 4};
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.boundingBox[0], 1023u);
  EXPECT_EQ(g_gxState.boundingBox[1], 0u);
  EXPECT_EQ(g_gxState.boundingBox[2], 1023u);
  EXPECT_EQ(g_gxState.boundingBox[3], 0u);
}

TEST_F(GXFifoTest, ReadBoundingBox_ReturnsTrackedState) {
  g_gxState.boundingBox = {10, 20, 30, 40};

  u16 left = 0;
  u16 right = 0;
  u16 top = 0;
  u16 bottom = 0;
  GXReadBoundingBox(&left, &right, &top, &bottom);

  EXPECT_EQ(left, 10u);
  EXPECT_EQ(right, 20u);
  EXPECT_EQ(top, 30u);
  EXPECT_EQ(bottom, 40u);
}

// --- Clear color and depth round-trip ---
TEST_F(GXFifoTest, CopyClear_ColorAndDepth) {
  GXColor color = {64, 128, 192, 255};
  GXSetCopyClear(color, 0x00ABCDEF);
  auto bytes = capture_fifo();

  // 3 BP writes: 3 * 5 = 15 bytes
  ASSERT_EQ(bytes.size(), 15u);
  EXPECT_EQ(bytes[0], 0x61);
  EXPECT_EQ(bytes[1], 0x4F); // R + A
  EXPECT_EQ(bytes[5], 0x61);
  EXPECT_EQ(bytes[6], 0x50); // B + G
  EXPECT_EQ(bytes[10], 0x61);
  EXPECT_EQ(bytes[11], 0x51); // Z

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.clearColor[0], 64.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[1], 128.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[2], 192.f / 255.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[3], 255.f / 255.f, 1.f / 255.f);
  EXPECT_EQ(g_gxState.clearDepth, 0x00ABCDEFu);
}

// --- Clear with black and zero depth ---
TEST_F(GXFifoTest, CopyClear_BlackZeroDepth) {
  GXColor color = {0, 0, 0, 0};
  GXSetCopyClear(color, 0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.clearColor[0], 0.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[1], 0.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[2], 0.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[3], 0.f, 1.f / 255.f);
  EXPECT_EQ(g_gxState.clearDepth, 0u);
}

// --- Clear with max depth ---
TEST_F(GXFifoTest, CopyClear_MaxDepth) {
  GXColor color = {255, 255, 255, 128};
  GXSetCopyClear(color, 0xFFFFFF);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_NEAR(g_gxState.clearColor[0], 1.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[1], 1.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[2], 1.f, 1.f / 255.f);
  EXPECT_NEAR(g_gxState.clearColor[3], 128.f / 255.f, 1.f / 255.f);
  EXPECT_EQ(g_gxState.clearDepth, 0xFFFFFFu);
}

TEST_F(GXFifoTest, PeekZ_ReturnsClearDepthFallbackAndRequestsSnapshot) {
  g_gxState.clearDepth = 0x123456;

  u32 z = 0;
  GXPeekZ(10, 20, &z);

  EXPECT_EQ(z, 0x123456u);
  EXPECT_TRUE(aurora::gfx::depth_peek::testing::snapshot_requested());
}

TEST_F(GXFifoTest, PeekZ_ReturnsLatestCompletedSnapshot) {
  aurora::gfx::depth_peek::testing::set_latest(2, 2, {0x000001, 0x000002, 0x000003, 0x01000004});

  u32 z = 0;
  GXPeekZ(1, 1, &z);

  EXPECT_EQ(z, 0x000004u);
  EXPECT_TRUE(aurora::gfx::depth_peek::testing::snapshot_requested());
}

TEST_F(GXFifoTest, PeekZ_OutOfRangeReturnsClearDepthFallback) {
  g_gxState.clearDepth = 0xabcdef;
  aurora::gfx::depth_peek::testing::set_latest(1, 1, {0x000001});

  u32 z = 0;
  GXPeekZ(1, 0, &z);

  EXPECT_EQ(z, 0xabcdefu);
  EXPECT_TRUE(aurora::gfx::depth_peek::testing::snapshot_requested());
}

// Composite tests (multiple state changes in a single FIFO stream)

TEST_F(GXFifoTest, Composite_BlendAndZMode) {
  GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
  GXSetZMode(true, GX_LEQUAL, true);
  GXSetAlphaCompare(GX_GREATER, 128, GX_AOP_AND, GX_ALWAYS, 0);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.blendMode, GX_BM_BLEND);
  EXPECT_EQ(g_gxState.blendFacSrc, GX_BL_SRCALPHA);
  EXPECT_EQ(g_gxState.blendFacDst, GX_BL_INVSRCALPHA);

  EXPECT_TRUE(g_gxState.depthCompare);
  EXPECT_EQ(g_gxState.depthFunc, GX_LEQUAL);
  EXPECT_TRUE(g_gxState.depthUpdate);

  EXPECT_EQ(g_gxState.alphaCompare.comp0, GX_GREATER);
  EXPECT_EQ(g_gxState.alphaCompare.ref0, 128u);
}

// --- GXLoadTexMtxImm for PTTexMtx (XF 0x500-0x5EF) ---

TEST_F(GXFifoTest, LoadPTTexMtx_Identity) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 1.0f;
  mtx.m1[1] = 1.0f;
  mtx.m2[2] = 1.0f;

  GXLoadTexMtxImm(&mtx, GX_PTTEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  // XF opcode 0x10, addr = (64 - 64) * 4 + 0x500 = 0x500, count = 12
  ASSERT_GE(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], 0x10);

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.ptTexMtxs[0];
  EXPECT_FLOAT_EQ(decoded.m0[0], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadPTTexMtx_ArbitraryValues) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 2.0f;  mtx.m0[1] = 0.5f;  mtx.m0[2] = 0.0f;  mtx.m0[3] = 10.0f;
  mtx.m1[0] = -0.5f; mtx.m1[1] = 3.0f;  mtx.m1[2] = 0.0f;  mtx.m1[3] = 20.0f;
  mtx.m2[0] = 0.0f;  mtx.m2[1] = 0.0f;  mtx.m2[2] = 1.5f;  mtx.m2[3] = -5.0f;

  GXLoadTexMtxImm(&mtx, GX_PTTEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.ptTexMtxs[0];
  EXPECT_FLOAT_EQ(decoded.m0[0], 2.0f);
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.5f);
  EXPECT_FLOAT_EQ(decoded.m0[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m0[3], 10.0f);
  EXPECT_FLOAT_EQ(decoded.m1[0], -0.5f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 3.0f);
  EXPECT_FLOAT_EQ(decoded.m1[2], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m1[3], 20.0f);
  EXPECT_FLOAT_EQ(decoded.m2[0], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 1.5f);
  EXPECT_FLOAT_EQ(decoded.m2[3], -5.0f);
}

TEST_F(GXFifoTest, LoadPTTexMtx_DifferentSlots) {
  aurora::Mat3x4<float> mtx0{};
  mtx0.m0[0] = 1.0f;
  mtx0.m1[1] = 1.0f;
  mtx0.m2[2] = 1.0f;

  aurora::Mat3x4<float> mtx5{};
  mtx5.m0[0] = 5.0f;
  mtx5.m1[1] = 6.0f;
  mtx5.m2[2] = 7.0f;
  mtx5.m0[3] = 100.0f;

  GXLoadTexMtxImm(&mtx0, GX_PTTEXMTX0, GX_MTX3x4);
  GXLoadTexMtxImm(&mtx5, GX_PTTEXMTX5, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Slot 0
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[0].m0[0], 1.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[0].m1[1], 1.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[0].m2[2], 1.0f);

  // Slot 5: GX_PTTEXMTX5 = 79, index = (79 - 64) / 3 = 5
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[5].m0[0], 5.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[5].m1[1], 6.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[5].m2[2], 7.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[5].m0[3], 100.0f);
}

TEST_F(GXFifoTest, LoadPTTexMtx_LastSlot) {
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 42.0f;
  mtx.m1[1] = 43.0f;
  mtx.m2[2] = 44.0f;

  GXLoadTexMtxImm(&mtx, GX_PTTEXMTX19, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  auto& decoded = g_gxState.ptTexMtxs[19];
  EXPECT_FLOAT_EQ(decoded.m0[0], 42.0f);
  EXPECT_FLOAT_EQ(decoded.m1[1], 43.0f);
  EXPECT_FLOAT_EQ(decoded.m2[2], 44.0f);
  // Other elements should be zero (from reset)
  EXPECT_FLOAT_EQ(decoded.m0[1], 0.0f);
  EXPECT_FLOAT_EQ(decoded.m2[3], 0.0f);
}

TEST_F(GXFifoTest, LoadPTTexMtx_Isolation) {
  // Loading PTTexMtx0 should not affect PTTexMtx1
  aurora::Mat3x4<float> mtx{};
  mtx.m0[0] = 99.0f;
  mtx.m1[1] = 88.0f;
  mtx.m2[2] = 77.0f;

  GXLoadTexMtxImm(&mtx, GX_PTTEXMTX0, GX_MTX3x4);
  auto bytes = capture_fifo();

  reset_gx_state();
  decode_fifo(bytes);

  // Slot 0 should have our values
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[0].m0[0], 99.0f);
  // Slot 1 should remain zeroed
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[1].m0[0], 0.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[1].m1[1], 0.0f);
  EXPECT_FLOAT_EQ(g_gxState.ptTexMtxs[1].m2[2], 0.0f);
}

// Composite / multi-command tests

TEST_F(GXFifoTest, Composite_TevSetup) {
  // Set up a simple 1-stage TEV that passes through texture color
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
  GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
  GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);

  // TEV order writes to dirty state, so flush before capture
  auto bytes = flush_and_capture();

  reset_gx_state();
  decode_fifo(bytes);

  EXPECT_EQ(g_gxState.numTevStages, 1u);
  EXPECT_EQ(g_gxState.tevStages[0].texMapId, GX_TEXMAP0);
  EXPECT_EQ(g_gxState.tevStages[0].texCoordId, GX_TEXCOORD0);
  EXPECT_EQ(g_gxState.tevStages[0].channelId, GX_COLOR0A0);
  EXPECT_EQ(g_gxState.tevStages[0].colorPass.d, GX_CC_TEXC);
  EXPECT_EQ(g_gxState.tevStages[0].alphaPass.d, GX_CA_TEXA);
}
