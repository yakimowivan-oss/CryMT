// Screen-space shadows for the sun: Bend Studio's "Screen Space Shadows" (SIGGRAPH 2023), bend_sss_cpu.h and
// bend_sss_gpu.h, used by CryMT's D3D12 back end (D3D12Backend.cpp, RunSss).
//
// Copyright 2023 Sony Interactive Entertainment.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The license text: BendSss.LICENSE.txt next to this file (in packages: licenses\bend_sss.license.txt).
//
// Modified for CryMT (2026-11-12), see the notes at kBendSssShader. Bend::BuildDispatchList is the original's
// (bend_sss_cpu.h), unchanged but for formatting; CryMT moves the sun's clip position by the frame's jitter before
// calling it and replaces the light's depth after it.
#pragma once

namespace Bend
{
// Generating a screen-space-shadow requires a number of Compute Shader dispatches. The compute shader reads from a depth
// buffer, and writes a single-channel texture of the same dimensions. Each dispatch is of the same compute shader. The
// number of dispatches required varies based on the on-screen location of the light: typically one or two when the light
// is off-screen, 4 to 6 when it is on-screen. Syncing the GPU between individual dispatches is not required.
struct DispatchData
{
    int WaveCount[3];        // Compute Shader Dispatch(X,Y,Z) wave counts X/Y/Z
    int WaveOffset_Shader[2]; // This value is passed in to shader. It will be different for each dispatch
};

struct DispatchList
{
    float LightCoordinate_Shader[4]; // This value is passed in to shader, the same for all dispatches for this light
    DispatchData Dispatch[8];        // List of dispatches (max count is 8)
    int DispatchCount;               // Number of compute dispatches written to the list
};

inline int bend_min(const int a, const int b) { return a > b ? b : a; }
inline int bend_max(const int a, const int b) { return a > b ? a : b; }

// inLightProjection: homogeneous coordinate of the light, {light} * {ViewProjectionMatrix} without the W divide; for
// infinite directional lights {light} = float4(normalized light direction, 0).
// inViewportSize: width/height of the render target.
// inMinRenderBounds / inMaxRenderBounds: 2D screen bounds of the light within the viewport, inclusive; [0,0], [width,height]
// for full-screen. The shader will still read/write outside of these bounds (by at most 2 * WAVE_SIZE pixels).
// inExpandedZRange: the API expects a [-1,+1] z range (typically false). inWaveSize: the compute shader's (64).
inline DispatchList BuildDispatchList(float inLightProjection[4], int inViewportSize[2], int inMinRenderBounds[2], int inMaxRenderBounds[2],
                                      bool inExpandedZRange = false, int inWaveSize = 64)
{
    DispatchList result = {};

    // Floating point division in the shader has a practical limit for precision when the light is *very* far off screen
    // (~1m pixels+), so when computing the light XY coordinate, use an adjusted w value to handle these extreme values.
    float xy_light_w = inLightProjection[3];
    float FP_limit = 0.000002f * (float)inWaveSize;

    if (xy_light_w >= 0 && xy_light_w < FP_limit)
        xy_light_w = FP_limit;
    else if (xy_light_w < 0 && xy_light_w > -FP_limit)
        xy_light_w = -FP_limit;

    // Need precise XY pixel coordinates of the light
    result.LightCoordinate_Shader[0] = ((inLightProjection[0] / xy_light_w) * +0.5f + 0.5f) * (float)inViewportSize[0];
    result.LightCoordinate_Shader[1] = ((inLightProjection[1] / xy_light_w) * -0.5f + 0.5f) * (float)inViewportSize[1];
    result.LightCoordinate_Shader[2] = inLightProjection[3] == 0 ? 0 : (inLightProjection[2] / inLightProjection[3]);
    result.LightCoordinate_Shader[3] = inLightProjection[3] > 0 ? 1 : -1;

    if (inExpandedZRange)
        result.LightCoordinate_Shader[2] = result.LightCoordinate_Shader[2] * 0.5f + 0.5f;

    int light_xy[2] = {(int)(result.LightCoordinate_Shader[0] + 0.5f), (int)(result.LightCoordinate_Shader[1] + 0.5f)};

    // Make the bounds inclusive, relative to the light
    const int biased_bounds[4] = {
        inMinRenderBounds[0] - light_xy[0],
        -(inMaxRenderBounds[1] - light_xy[1]),
        inMaxRenderBounds[0] - light_xy[0],
        -(inMinRenderBounds[1] - light_xy[1]),
    };

    // Process 4 quadrants around the light center. They each form a rectangle with one corner on the light XY coordinate.
    // If the rectangle isn't square, it will need breaking in two on the larger axis.
    // 0 = bottom left, 1 = bottom right, 2 = top left, 2 = top right
    for (int q = 0; q < 4; q++)
    {
        // Quads 0 and 3 needs to be +1 vertically, 1 and 2 need to be +1 horizontally
        bool vertical = q == 0 || q == 3;

        // Bounds relative to the quadrant
        const int bounds[4] = {
            bend_max(0, ((q & 1) ? biased_bounds[0] : -biased_bounds[2])) / inWaveSize,
            bend_max(0, ((q & 2) ? biased_bounds[1] : -biased_bounds[3])) / inWaveSize,
            bend_max(0, (((q & 1) ? biased_bounds[2] : -biased_bounds[0]) + inWaveSize * (vertical ? 1 : 2) - 1)) / inWaveSize,
            bend_max(0, (((q & 2) ? biased_bounds[3] : -biased_bounds[1]) + inWaveSize * (vertical ? 2 : 1) - 1)) / inWaveSize,
        };

        if ((bounds[2] - bounds[0]) > 0 && (bounds[3] - bounds[1]) > 0)
        {
            int bias_x = (q == 2 || q == 3) ? 1 : 0;
            int bias_y = (q == 1 || q == 3) ? 1 : 0;

            DispatchData& disp = result.Dispatch[result.DispatchCount++];

            disp.WaveCount[0] = inWaveSize;
            disp.WaveCount[1] = bounds[2] - bounds[0];
            disp.WaveCount[2] = bounds[3] - bounds[1];
            disp.WaveOffset_Shader[0] = ((q & 1) ? bounds[0] : -bounds[2]) + bias_x;
            disp.WaveOffset_Shader[1] = ((q & 2) ? -bounds[3] : bounds[1]) + bias_y;

            // We want the far corner of this quadrant relative to the light, as we need to know where the diagonal light
            // ray intersects with the edge of the bounds
            int axis_delta = +biased_bounds[0] - biased_bounds[1];
            if (q == 1)
                axis_delta = +biased_bounds[2] + biased_bounds[1];
            if (q == 2)
                axis_delta = -biased_bounds[0] - biased_bounds[3];
            if (q == 3)
                axis_delta = -biased_bounds[2] + biased_bounds[3];

            axis_delta = (axis_delta + inWaveSize - 1) / inWaveSize;

            if (axis_delta > 0)
            {
                DispatchData& disp2 = result.Dispatch[result.DispatchCount++];

                // Take copy of current volume
                disp2 = disp;

                if (q == 0)
                {
                    // Split on Y, split becomes -1 larger on x
                    disp2.WaveCount[2] = bend_min(disp.WaveCount[2], axis_delta);
                    disp.WaveCount[2] -= disp2.WaveCount[2];
                    disp2.WaveOffset_Shader[1] = disp.WaveOffset_Shader[1] + disp.WaveCount[2];
                    disp2.WaveOffset_Shader[0]--;
                    disp2.WaveCount[1]++;
                }
                if (q == 1)
                {
                    // Split on X, split becomes +1 larger on y
                    disp2.WaveCount[1] = bend_min(disp.WaveCount[1], axis_delta);
                    disp.WaveCount[1] -= disp2.WaveCount[1];
                    disp2.WaveOffset_Shader[0] = disp.WaveOffset_Shader[0] + disp.WaveCount[1];
                    disp2.WaveCount[2]++;
                }
                if (q == 2)
                {
                    // Split on X, split becomes -1 larger on y
                    disp2.WaveCount[1] = bend_min(disp.WaveCount[1], axis_delta);
                    disp.WaveCount[1] -= disp2.WaveCount[1];
                    disp.WaveOffset_Shader[0] += disp2.WaveCount[1];
                    disp2.WaveCount[2]++;
                    disp2.WaveOffset_Shader[1]--;
                }
                if (q == 3)
                {
                    // Split on Y, split becomes +1 larger on x
                    disp2.WaveCount[2] = bend_min(disp.WaveCount[2], axis_delta);
                    disp.WaveCount[2] -= disp2.WaveCount[2];
                    disp.WaveOffset_Shader[1] += disp2.WaveCount[2];
                    disp2.WaveCount[1]++;
                }

                // Remove if too small
                if (disp2.WaveCount[1] <= 0 || disp2.WaveCount[2] <= 0)
                    disp2 = result.Dispatch[--result.DispatchCount];
                if (disp.WaveCount[1] <= 0 || disp.WaveCount[2] <= 0)
                    disp = result.Dispatch[--result.DispatchCount];
            }
        }
    }

    // Scale the shader values by the wave count, the shader expects this
    for (int i = 0; i < result.DispatchCount; i++)
    {
        result.Dispatch[i].WaveOffset_Shader[0] *= inWaveSize;
        result.Dispatch[i].WaveOffset_Shader[1] *= inWaveSize;
    }

    return result;
}
} // namespace Bend

// The compute shader: bend_sss_gpu.h's WriteScreenSpaceShadow, modified for CryMT:
// - FXC cs_5_0 (the game's d3dcompiler_47): global resources and root constants (b0) in place of the DispatchParameters
//   struct; the tuning switches are compile-time defines (SAMPLE_COUNT, HARD_SHADOW_SAMPLES, FADE_OUT_SAMPLES,
//   IGNORE_EDGE_PIXELS, BILINEAR_OFFSET_MODE); no debug outputs, no precision offset;
// - the depth is the game's z-prepass target (linear: view depth / far plane), read with Load (0 outside the texture: far)
//   and turned into 1 / depth. That is a reversed infinite projection's depth times a constant: a world line stays straight
//   on screen with this depth linear along it, the far value is 0, near ones are larger (z_sign -1), and the sun (at
//   infinity) has depth 0;
// - no wave intrinsics (not in SM 5): no group-wide early-out; a pixel outside [near, reach] metres (the sky, far away)
//   is written as unshadowed after the group's depth samples are shared;
// - grass and leaves (the z-prepass's foliage mask, t1) cast nothing unless FoliageCasts;
// - the first-person view (weapon, hands: drawn with their own camera into the hardware depth under 0.1, t2) takes its
//   own thickness: its objects lie centimetres apart at well under a metre, where 0.5% of the depth is a few millimetres
//   (the weapon cast no shadow on the hands, the watch's small parts did);
// - the result is the occlusion (1 - the original's value: 1 shadowed), so the shaders' placeholder (zero) means none,
//   faded out over the last quarter of the reach.
static const char* const kBendSssShader = R"(
#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 60
#endif
#ifndef HARD_SHADOW_SAMPLES
#define HARD_SHADOW_SAMPLES 4
#endif
#ifndef FADE_OUT_SAMPLES
#define FADE_OUT_SAMPLES 8
#endif
#ifndef IGNORE_EDGE_PIXELS
#define IGNORE_EDGE_PIXELS 0
#endif
#ifndef BILINEAR_OFFSET_MODE
#define BILINEAR_OFFSET_MODE 0
#endif
#define WAVE_SIZE 64
#define READ_COUNT (SAMPLE_COUNT / WAVE_SIZE + 2)

Texture2D<float> CryDepth : register(t0);   // the z-prepass's linear depth
Texture2D<float> CryFoliage : register(t1); // the foliage mask (1 leaves, 0.6 grass)
Texture2D<float> CryHwDepth : register(t2); // the scene's depth buffer: the first-person view lies under 0.1
RWTexture2D<unorm float> CryOut : register(u0);

cbuffer CrySss : register(b0)
{
  float4 LightCoordinate;  // BuildDispatchList's: x, y pixels; z the light's depth (0); w +1 / -1
  int2 WaveOffset;         // the dispatch's
  int2 DepthSize;
  float SurfaceThickness;  // a pixel's assumed thickness, a share of its depth (Bend: 0.005)
  float BilinearThreshold; // a depth step taken as an edge, a share of the depth (0.02)
  float ShadowContrast;    // (4)
  float FarMeters;         // metres per unit of the linear depth
  float ReachMeters;       // pixels farther get no shadow (faded over the last quarter)
  float FoliageCasts;      // 1: the foliage mask is not read
  float NearMeters;        // pixels nearer get no shadow
  float WeaponThickness;   // SurfaceThickness for the first-person view (SurfaceThickness without the depth buffer)
};

groupshared float DepthData[READ_COUNT * WAVE_SIZE];

// 1 / the linear depth (0: far, outside the texture).
float InvDepth(int2 p)
{
  float z = CryDepth.Load(int3(p, 0));
  return z > 0 ? 1 / z : 0;
}

// Gets the start pixel coordinates for the pixels in the wavefront. Also returns the delta to get to the next pixel after
// WAVE_COUNT pixels along the ray.
void ComputeWavefrontExtents(int3 inGroupID, uint inGroupThreadID, out float2 outDeltaXY, out float2 outPixelXY, out float outPixelDistance, out bool outMajorAxisX)
{
  int2 xy = inGroupID.yz * WAVE_SIZE + WaveOffset.xy;

  // integer light position / fractional component
  float2 light_xy = floor(LightCoordinate.xy) + 0.5;
  float2 light_xy_fraction = LightCoordinate.xy - light_xy;
  bool reverse_direction = LightCoordinate.w > 0.0f;

  int2 sign_xy = sign(xy);
  bool horizontal = abs(xy.x + sign_xy.y) < abs(xy.y - sign_xy.x);

  int2 axis;
  axis.x = horizontal ? (+sign_xy.y) : (0);
  axis.y = horizontal ? (0) : (-sign_xy.x);

  // Apply wave offset
  xy = axis * (int)inGroupID.x + xy;
  float2 xy_f = (float2)xy;

  // For interpolation to the light center, we only really care about the larger of the two axis
  bool x_axis_major = abs(xy_f.x) > abs(xy_f.y);
  float major_axis = x_axis_major ? xy_f.x : xy_f.y;

  float major_axis_start = abs(major_axis);
  float major_axis_end = abs(major_axis) - (float)WAVE_SIZE;

  float ma_light_frac = x_axis_major ? light_xy_fraction.x : light_xy_fraction.y;
  ma_light_frac = major_axis > 0 ? -ma_light_frac : ma_light_frac;

  // back in to screen direction
  float2 start_xy = xy_f + light_xy;

  // For the very inner most ring, we need to interpolate to a pixel centered UV, so the UV->pixel rounding doesn't skip
  // output pixels
  float2 end_xy = lerp(LightCoordinate.xy, start_xy, (major_axis_end + ma_light_frac) / (major_axis_start + ma_light_frac));

  // The major axis should be a round number
  float2 xy_delta = (start_xy - end_xy);

  // Inverse the read order when reverse direction is true
  float thread_step = (float)(inGroupThreadID ^ (reverse_direction ? 0 : (WAVE_SIZE - 1)));

  float2 pixel_xy = lerp(start_xy, end_xy, thread_step / (float)WAVE_SIZE);
  float pixel_distance = major_axis_start - thread_step + ma_light_frac;

  outPixelXY = pixel_xy;
  outPixelDistance = pixel_distance;
  outDeltaXY = xy_delta;
  outMajorAxisX = x_axis_major;
}

// The shadow test against the group's shared samples for one thickness window (Bend's main loop, unchanged): 1 lit,
// 0 shadowed. depth_scale: the window's inverse size for the pixel (see main).
float Visibility(float start_depth, float depth_scale, int sample_index)
{
  int i;
  float4 shadow_value = 1;
  float hard_shadow = 1;
  start_depth = start_depth * depth_scale + 1; // - z_sign (-1: near over far)

  // The first number of hard shadow samples, a single pixel can produce a full shadow
  [unroll] for (i = 0; i < HARD_SHADOW_SAMPLES; i++)
  {
    float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);

    // We want to find the distance of the sample that is closest to the reference depth
    hard_shadow = min(hard_shadow, depth_delta);
  }

  // Brute force go! The main shadow samples, averaged in to a set of 4 shadow values
  [unroll] for (i = HARD_SHADOW_SAMPLES; i < SAMPLE_COUNT - FADE_OUT_SAMPLES; i++)
  {
    float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);

    // Do the same as the hard_shadow code above, but this will accumulate to 4 separate values. By using 4 values, the
    // average shadow can be taken, which can help soften single-pixel shadows.
    shadow_value[i & 3] = min(shadow_value[i & 3], depth_delta);
  }

  // Final fade out samples
  [unroll] for (i = SAMPLE_COUNT - FADE_OUT_SAMPLES; i < SAMPLE_COUNT; i++)
  {
    float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);

    // Add the fade value to these samples
    const float fade_out = (float)(i + 1 - (SAMPLE_COUNT - FADE_OUT_SAMPLES)) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;

    shadow_value[i & 3] = min(shadow_value[i & 3], depth_delta + fade_out);
  }

  // Apply the contrast value. A value of 0 indicates a sample was exactly matched to the reference depth (and the result
  // is fully shadowed). We want some boost to this range, so samples don't have to exactly match to produce a full shadow.
  shadow_value = saturate(shadow_value * ShadowContrast + (1 - ShadowContrast));
  hard_shadow = saturate(hard_shadow * ShadowContrast + (1 - ShadowContrast));

  // Take the average of 4 samples, this is useful to reduces aliasing noise in the source depth, especially with long
  // shadows. If the first samples are always producing a hard shadow, then compute this value separately.
  return min(hard_shadow, dot(shadow_value, 0.25));
}

[numthreads(WAVE_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
  const int3 inGroupID = (int3)groupId;
  const uint inGroupThreadID = groupThreadId.x;

  float2 xy_delta;
  float2 pixel_xy;
  float pixel_distance;
  bool x_axis_major; // major axis is x axis? abs(xy_delta.x) > abs(xy_delta.y).

  ComputeWavefrontExtents(inGroupID, inGroupThreadID, xy_delta, pixel_xy, pixel_distance, x_axis_major);

  // Read in the depth values
  float sampling_depth[READ_COUNT];
  float shadowing_depth[READ_COUNT];
  float depth_thickness_scale[READ_COUNT];
  float sample_distance[READ_COUNT];

  const float direction = -LightCoordinate.w;
  const float z_sign = -1; // near (large 1 / depth) over far (0)

  int i;
  const float2 write_xy = floor(pixel_xy);

  [unroll] for (i = 0; i < READ_COUNT; i++)
  {
    // We sample depth twice per pixel per sample, and interpolate with an edge detect filter. Interpolation should only
    // occur on the minor axis of the ray - major axis coordinates should be at pixel centers.
    int2 read_xy = (int2)floor(pixel_xy);
    float minor_axis = x_axis_major ? pixel_xy.y : pixel_xy.x;

    // If a pixel has been detected as an edge, then optionally don't include it in the shadow
    const float edge_skip = 1e20; // an extreme value pushes edge samples out of range

    float2 depths;
    float bilinear = frac(minor_axis) - 0.5;
    int bias = bilinear > 0 ? 1 : -1;
    int2 offset_xy = int2(x_axis_major ? 0 : bias, x_axis_major ? bias : 0);
    depths.x = InvDepth(read_xy);
    depths.y = InvDepth(read_xy + offset_xy);

    // Depth thresholds (bilinear/shadow thickness) are based on a fractional ratio of the difference between sampled
    // depth and the far clip depth (0)
    depth_thickness_scale[i] = abs(depths.x);

    // If depth variance is more than a specific threshold, then just use point filtering
    bool use_point_filter = abs(depths.x - depths.y) > depth_thickness_scale[i] * BilinearThreshold;

#if BILINEAR_OFFSET_MODE
    bilinear = use_point_filter ? 0 : bilinear;
    // both shadow depth and starting depth are the same in this mode, unless shadow skipping edges
    sampling_depth[i] = lerp(depths.x, depths.y, abs(bilinear));
    shadowing_depth[i] = (IGNORE_EDGE_PIXELS && use_point_filter) ? edge_skip : sampling_depth[i];
#else
    // The pixel starts sampling at this depth
    sampling_depth[i] = depths.x;

    float edge_depth = IGNORE_EDGE_PIXELS ? edge_skip : depths.x;
    // Any sample in this wavefront is possibly interpolated towards the bilinear sample, so use a shadowing depth that is
    // further away, based on the difference between the two samples
    float shadow_depth = depths.x + abs(depths.x - depths.y) * z_sign;

    // Shadows cast from this depth
    shadowing_depth[i] = use_point_filter ? edge_depth : shadow_depth;
#endif

    // CryMT: grass and leaves cast nothing unless asked
    [branch] if (FoliageCasts < 0.5 && CryFoliage.Load(int3(read_xy, 0)) > 0.5)
      shadowing_depth[i] = edge_skip;

    // Store for later
    sample_distance[i] = pixel_distance + (WAVE_SIZE * i) * direction;

    // Iterate to the next pixel along the ray. This will be WAVE_SIZE pixels along the ray...
    pixel_xy += xy_delta * direction;
  }

  // CryMT: the pixel's own distance; no shadow outside [near, reach] (the sky, far away).
  const float start_meters = sampling_depth[0] > 0 ? FarMeters / sampling_depth[0] : 1e30;
  const bool skip_pixel = !(start_meters <= ReachMeters) || start_meters < NearMeters;

  // Write the shadow depths to LDS
  [unroll] for (i = 0; i < READ_COUNT; i++)
  {
    // Perspective correct the shadowing depth, in this space, all light rays are parallel
    float stored_depth = (shadowing_depth[i] - LightCoordinate.z) / sample_distance[i];

    if (i != 0)
    {
      // For pixels within sampling distance of the light, it is possible that sampling will overshoot the light
      // coordinate for extended reads. We want to ignore these samples
      stored_depth = sample_distance[i] > 0 ? stored_depth : 1e10;
    }

    // Store the depth values in groupshared
    DepthData[(i * WAVE_SIZE) + inGroupThreadID] = stored_depth;
  }

  // Sync wavefronts now groupshared DepthData is written
  GroupMemoryBarrierWithGroupSync();

  // If the starting depth isn't in depth bounds, then we don't need a shadow
  if (skip_pixel)
  {
    CryOut[(int2)write_xy] = 0;
    return;
  }

  // perspective correct the depth
  float start_depth = (sampling_depth[0] - LightCoordinate.z) / sample_distance[0];

  // Start by reading the next value
  int sample_index = inGroupThreadID + 1;

  // This is the inverse of how large the shadowing window is for the projected sample data. All values in the LDS sample
  // list are scaled by 1.0 / sample_distance, such that all light directions become parallel. The multiply by
  // sample_distance[0] here is to compensate for the projection divide in the data. The 1.0 / SurfaceThickness is to
  // adjust user selected thickness. 1.0 / depth_thickness_scale[0] is because SurfaceThickness is percentage of remaining
  // depth between the sample and the far clip - not a percentage of the full depth range. The min() function is to make
  // sure the window is a minimum width when very close to the light. The +direction term will bias the result so the
  // pixel at the very center of the light is either fully lit or shadowed
  const float scale_base = sample_distance[0] / depth_thickness_scale[0];
  float result = Visibility(start_depth, min(sample_distance[0] + direction, 1.0 / SurfaceThickness) * scale_base, sample_index);

  // CryMT: the first-person view (the hardware depth under 0.1) also with its own, thicker window, the darker result: a
  // window takes occluders 0.25 to 2 thicknesses above the ray, so the world's (a few millimetres at half a metre) missed
  // the weapon over the hands, and a thick one alone lost the small parts (the watch: shadows on the weapon fell 17%).
  [branch] if (WeaponThickness != SurfaceThickness && CryHwDepth.Load(int3((int2)write_xy, 0)) < 0.1)
    result = min(result, Visibility(start_depth, min(sample_distance[0] + direction, 1.0 / WeaponThickness) * scale_base, sample_index));

  // CryMT: skin (HumanSkin's z-prepass marks it 0.3 in the foliage mask) receives none for now (the user, 2026-11-15:
  // to be reworked later); it still casts.
  float cryMark = CryFoliage.Load(int3((int2)write_xy, 0));
  if (cryMark > 0.2 && cryMark < 0.4)
    result = 1;

  // CryMT: written as the occlusion, faded out over the reach's last quarter
  CryOut[(int2)write_xy] = (1 - result) * saturate((ReachMeters - start_meters) / (0.25 * ReachMeters));
}
)";
