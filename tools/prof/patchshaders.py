# Applies CryMT's shader changes to the game's own shader sources (Game\Shaders.pak) and writes the patched files
# plus CryMT's GI placeholder texture, so CryMT ships edits, not Crytek's files.
# Usage: python patchshaders.py <Shaders.pak> <out dir>
import os
import struct
import sys
import zipfile

# The engine binds Textures/CryMT/gi.dds like any texture of a sampler; CryMT's D3D12 back end recognizes it and shows
# its GI texture in its place. Engine syntax only: the FX parser rejected a raw "register(t16)" declaration.
GI_DECL = '''#include "CommonRAM.cfi"

// CryMT GI: the engine binds the placeholder Textures/CryMT/gi.dds (all zero: the ambient stays as it was); CryMT's
// D3D12 back end shows its GI texture in its place. rgb = indirect light added, a = share of the engine's ambient
// replaced (sky occlusion) plus flags (below).
#if D3D10
sampler2D CryMT_GISampler = sampler_state
{
  Texture = Textures/CryMT/gi.dds;
  MinFilter = LINEAR;
  MagFilter = LINEAR;
  MipFilter = POINT;
  AddressU = Clamp;
  AddressV = Clamp;
};

// a = share + 2 where CryMT's GI is in effect. Which of the engine's terms it replaces: the top-left texel's a / 2
// (bits: 1 SSAO, 2 the objects' RAM, 4 their sky amount, 8 TerrainAO, 16 the terrain's vertex AO; its rgb 0). The
// placeholder (no GI) is 0: everything as the engine has it.
half4 CryMT_GI(float4 screenProj)
{
  return tex2D(CryMT_GISampler, screenProj.xy / screenProj.w);
}

half CryMT_GIFlags(half w)
{
  return floor(w * 0.5);
}

half CryMT_GIShare(half w)
{
  return saturate(w - 2 * CryMT_GIFlags(w));
}

half3 CryMT_GIAmbient(half3 ambient, float4 screenProj)
{
  half4 gi = CryMT_GI(screenProj);
  return ambient * (1 - CryMT_GIShare(gi.w)) + gi.xyz;
}

// The share of the sky the GI sees (1 - its occlusion), for all specular (specular occlusion): the environment map's
// reflection is shaded by the ambient and diffuse light only, and shadow maps miss the inner faces of one-layer roofs
// and walls, so metal glinted in shadowed rooms.
half CryMT_GISpecular(float4 screenProj)
{
  return 1 - CryMT_GIShare(CryMT_GI(screenProj).w);
}

// The top-left texel's a / 2: the mask of the engine's terms the GI replaces (bits below; 32: the particles' GI,
// Particles.cfi) plus 64 x half the distance (metres) from which the engine's TerrainAO takes over from the GI again.
float CryMT_GIControl()
{
  return floor(tex2Dlod(CryMT_GISampler, float4(0, 0, 0, 0)).w * 0.5);
}

// Whether the GI replaces the engine's term `bit` at this pixel.
bool CryMT_GIReplaces(float4 screenProj, half bit)
{
  float mask = fmod(CryMT_GIControl(), 64);
  return CryMT_GIFlags(CryMT_GI(screenProj).w) > 0.5 && fmod(floor(mask / bit), 2) > 0.5;
}

// The engine's TerrainAO (under the trees; a 2.5D map: one canopy height per terrain cell): near the camera the GI's
// own occlusion takes its place (bit 8), from the distance in the top-left texel on, over half that again, the
// engine's (far trees are sprites, beyond the voxels).
half CryMT_GITerrainAOMix(float4 screenProj, half tao)
{
  float c = CryMT_GIControl();
  float start = floor(c / 64) * 2;
  if (!CryMT_GIReplaces(screenProj, 8))
    return tao;
  return lerp(1, tao, saturate((screenProj.w - start) / (0.5 * start + 1)));
}

// The engine's SSAO term, or 1 where the GI's occlusion replaces it.
half CryMT_GISSAOMix(float4 screenProj, half ssao)
{
  return CryMT_GIReplaces(screenProj, 1) ? 1 : ssao;
}

// A baked occlusion factor (bit: 2 RAM, 4 sky amount, 8 TerrainAO, 16 the terrain's vertex AO), or 1 where the GI
// replaces it.
half CryMT_GIBakedMix(float4 screenProj, half baked, half bit)
{
  return CryMT_GIReplaces(screenProj, bit) ? 1 : baked;
}
#else
half3 CryMT_GIAmbient(half3 ambient, float4 screenProj)
{
  return ambient;
}

half CryMT_GISpecular(float4 screenProj)
{
  return 1;
}

half CryMT_GISSAOMix(float4 screenProj, half ssao)
{
  return ssao;
}

half CryMT_GIBakedMix(float4 screenProj, half baked, half bit)
{
  return baked;
}

half CryMT_GITerrainAOMix(float4 screenProj, half tao)
{
  return tao;
}
#endif
'''

# All specular (lights and the environment map's reflection): also by the sky the GI sees.
GI_SPEC = '''  half3 cSpecular = pPass.cSpecularAcc.xyz * pPass.cGlossMap.xyz;                    // 2 alu'''
GI_SPEC_OCC = '''  half3 cSpecular = pPass.cSpecularAcc.xyz * pPass.cGlossMap.xyz;                    // 2 alu
#if !%_RT_FOG
  cSpecular *= CryMT_GISpecular(pPass.IN.screenProj); // CryMT GI: specular occlusion (lights and environment map)
#endif'''

# After frag_custom_ambient, so shaders that replace the ambient there (vegetation) get it too; before SSAO.
GI_AMBIENT = '''  // custom ambient pass
  frag_custom_ambient(pPass, amb);

#if !%_RT_FOG
  pPass.cAmbientAcc.xyz = CryMT_GIAmbient(pPass.cAmbientAcc.xyz, pPass.IN.screenProj);
#endif'''

TERRAIN_BASE = '''AmbientOp.xyz * IN.Color.a * (vAO.x * vAO.y * vAO.w)); // ambient'''
TERRAIN_BASE_GI = ('''CryMT_GIAmbient(AmbientOp.xyz * CryMT_GIBakedMix(IN.screenProj, IN.Color.a, 16), IN.screenProj) * '''
                   '''(CryMT_GISSAOMix(IN.screenProj, vAO.x) * CryMT_GITerrainAOMix(IN.screenProj, vAO.y) * vAO.w)); // ambient (CryMT GI)''')

JITTER_DECL = '''
#if D3D10
sampler2D CryMT_JitterSampler = sampler_state
{
  Texture = Textures/CryMT/jitter.dds;
  MinFilter = POINT;
  MagFilter = POINT;
  MipFilter = POINT;
  AddressU = Clamp;
  AddressV = Clamp;
};

// CryMT's foliage mask (the z-prepass's second target: 1 on alpha-tested surfaces), in place of this placeholder.
sampler2D CryMT_FoliageSampler = sampler_state
{
  Texture = Textures/CryMT/foliage.dds;
  MinFilter = POINT;
  MagFilter = POINT;
  MipFilter = POINT;
  AddressU = Clamp;
  AddressV = Clamp;
};

// CryMT's frame texture, 8x1 (the placeholder's texels are zero but the last, CryMT's mark): 0 the camera jitter
// (fractions of the screen) and the frame (0..63); 1 towards the sun, w 2 with contact shadows; 2..5 the columns of
// the unjittered view-projection; 6 the contact shadows' length and thickness at 10 m, their reach (metres), w 1:
// foliage casts them too.
float4 CryMT_Frame(float i)
{
  return tex2Dlod(CryMT_JitterSampler, float4((i + 0.5) / 8, 0.5, 0, 0));
}

// The frame's jitter in pixels of this pass's target (fractions of the screen over the screen coordinate's step).
float2 CryMT_JitterPixels(float2 screenTC)
{
  return CryMT_Frame(0).xy / float2(ddx(screenTC.x), ddy(screenTC.y));
}

// Contact shadows: from the pixel's point towards the sun, 12 steps over a length growing with distance, against the
// depth buffer: a step behind the depth there (by more than the surface's own depth step, less than a thickness) is
// shadowed. 1 shadowed, fading over the last part of the length and of the reach.
half CryMT_ContactShadow(float3 viewVect, float sceneDepth, float2 screenTC, float2 wpos)
{
  float2 jit = CryMT_JitterPixels(screenTC);
  float3 ray = viewVect - jit.x * ddx(viewVect) - jit.y * ddy(viewVect);
  float2 texel = float2(ddx(screenTC.x), ddy(screenTC.y));
  float4 sun = CryMT_Frame(1);
  float4 prm = CryMT_Frame(6);
  if (sun.w < 1.5 || sceneDepth <= 0)
    return 0;
  float4 f0 = CryMT_Frame(0);
  float4 c0 = CryMT_Frame(2), c1 = CryMT_Frame(3), c3 = CryMT_Frame(5);
  float3 P = vfViewPos.xyz + ray * sceneDepth;
  float w0 = dot(float4(P, 1), c3);
  if (w0 < 0.3 || w0 > prm.z)
    return 0;
  float zScale = w0 / sceneDepth;
  // The surface's depth step to a neighbour pixel (the smaller side of each axis: not across an edge).
  float zl = tex2Dlod(sceneDepthSampler, float4(screenTC - float2(texel.x, 0), 0, 0)).r;
  float zr = tex2Dlod(sceneDepthSampler, float4(screenTC + float2(texel.x, 0), 0, 0)).r;
  float zu = tex2Dlod(sceneDepthSampler, float4(screenTC - float2(0, texel.y), 0, 0)).r;
  float zd = tex2Dlod(sceneDepthSampler, float4(screenTC + float2(0, texel.y), 0, 0)).r;
  float slope = max(min(abs(zl - sceneDepth), abs(zr - sceneDepth)), min(abs(zu - sceneDepth), abs(zd - sceneDepth))) * zScale;
  float k = clamp(w0 / 10, 0.3, 3);
  float len = prm.x * k;
  float bias = 1.5 * slope + 0.002 * w0 + 0.01;
  float thick = max(prm.y * k, 2 * bias);
  float noise = frac(52.9829189 * frac(dot(wpos + 5.588238 * f0.z, float2(0.06711056, 0.00583715))));
  half occ = 0;
  for (int i = 0; i < 12; i++)
  {
    float t = (i + noise) / 12;
    float4 Q = float4(P + sun.xyz * (t * len), 1);
    float qw = dot(Q, c3);
    float2 uv = float2(dot(Q, c0), -dot(Q, c1)) / qw * 0.5 + 0.5 + f0.xy;
    if (qw <= 0 || any(uv != saturate(uv)))
      break;
    float dz = qw - tex2Dlod(sceneDepthSampler, float4(uv, 0, 0)).r * zScale;
    // Foliage (leaves, grass: the mask) casts none unless CRYMT_CONTACTFOLIAGE=1 (frame texel 6 w): grass showed
    // floating over the ground and darkened whole meadows at a low sun.
    if (dz > bias + 0.02 * t * len && dz < thick && (prm.w > 0.5 || tex2Dlod(CryMT_FoliageSampler, float4(uv, 0, 0)).r < 0.5))
    {
      occ = 1 - smoothstep(0.6, 1, t);
      break;
    }
  }
  return occ * saturate((prm.z - w0) / (0.25 * prm.z));
}
#else
float2 CryMT_JitterPixels(float2 screenTC)
{
  return 0;
}

half CryMT_ContactShadow(float3 viewVect, float sceneDepth, float2 screenTC, float2 wpos)
{
  return 0;
}
#endif
'''

PATCHES = {
    'shaders/hwscripts/cryfx/fraglib.cfi': [
        ('#include "CommonRAM.cfi"\n', GI_DECL),
        ('  // custom ambient pass\n  frag_custom_ambient(pPass, amb);', GI_AMBIENT),
        (GI_SPEC, GI_SPEC_OCC),
        # The baked occlusion (RAM, sky accessibility) and the SSAO term mixed by the GI's flags (Settings.cpp InstallGi).
        ('    amb.xyz\t*=\tpPass.cBumpMap.z*RamTex;', '    amb.xyz\t*=\tCryMT_GIBakedMix(pPass.IN.screenProj, pPass.cBumpMap.z*RamTex, 2);'),
        ('    amb.xyz *= GetSkyAmount(', '    amb.xyz *= CryMT_GIBakedMix(pPass.IN.screenProj, GetSkyAmount('),
        ('    pPass.IN.screenProj.xyzw);\n #else', '    pPass.IN.screenProj.xyzw), 4);\n #else'),
        ('    pPass.cAOMapVal = vAO.x*vAO.y*vAO.w;',
         '    pPass.cAOMapVal = CryMT_GISSAOMix(pPass.IN.screenProj, vAO.x) * CryMT_GITerrainAOMix(pPass.IN.screenProj, vAO.y) * vAO.w; // CryMT GI'),
    ],
    'shaders/hwscripts/cryfx/terrain.cfx': [
        (TERRAIN_BASE, TERRAIN_BASE_GI),
    ],
    # Blended particles (smoke, dust) take the GI for their ambient (Settings.cpp InstallGi).
    'shaders/hwscripts/cryfx/particles.cfi': [
        ('pixout ParticlePS(v2f IN)\n',
         '// CryMT GI for blended particles (smoke, dust), from the D3D12 back end\'s volumes around the camera (in place of\n// Textures/CryMT/pvol.dds): per cell x the share of ambient a particle there loses (outdoors a part of it, capped), y\n// the sun\'s visibility through the voxels (-1: no sun), z the indoor share; and the tint of the light the voxels bring\n// there (hue only: luminance 1, part white). The game lit particles by the sun with the screen\'s shadow mask behind\n// each pixel (and our first try took the GI there too): both flipped as dust crossed lit and shaded backgrounds (a\n// sunlit floor, a dark wall, a window). The voxels\' sun is taken indoors (by the indoor share); outdoors the game\'s (the\n// voxels\' grass and coarse ground cells shaded dust in the open sun). Two volumes of 64 x 64 x 32 cells: fine (0.5 m:\n// walls and ceilings of rooms) and coarse (1.5 m, +-48 m), each as z-slices tiled 8 across 512 x 256 of a 1024 x 513\n// atlas (RGBA32F: fine left, coarse right; rows 0-255 the shares, 257-512 the tints); row 256\'s texels 0-3 and 4-7 their\n// first cell relative to the camera (metres) and cell size. Read once per pixel (CryMT_PVolAt at the shader\'s start):\n// two slices blended per volume; the fine one where it reaches, over its outer cells to the coarse one, which fades out\n// over its own (then the game\'s own). The ambient is darkened and tinted, not brightened. Additive particles (fire,\n// sparks, flashes) are left as they are. The placeholder (no CryMT, or the effect off: key "-") has cell sizes of 0: the\n// game\'s own. The pixel shader is renamed so no cached build of the old one is used.\nstruct CryMT_PVol\n{\n  float share;     // of the ambient that goes\n  float3 tint;     // the ambient\'s tint\n  float sun;       // the sun\'s visibility (the voxels\')\n  float sunWeight; // how much of it to take (indoors, inside the volumes); 0: the game\'s own\n};\n\n#if D3D10\nsampler2D CryMT_PVolSampler = sampler_state\n{\n  Texture = Textures/CryMT/pvol.dds;\n  MinFilter = LINEAR;\n  MagFilter = LINEAR;\n  MipFilter = POINT;\n  AddressU = Clamp;\n  AddressV = Clamp;\n};\n\n// One volume (params at row 256\'s texels p .. p + 3, slices from atlas column x0): its share, sun and indoor share, its\n// tint, its weight (0 outside, rising over the outer 4 cells).\nvoid CryMT_PVolOne(float3 pos, float p, float x0, out float3 v, out float3 tint, out float weight)\n{\n  v = 0;\n  tint = 1;\n  weight = 0;\n  float cell = tex2Dlod(CryMT_PVolSampler, float4((p + 3.5) / 1024, 256.5 / 513, 0, 0)).x;\n  if (cell <= 0)\n    return;\n  float3 first = float3(tex2Dlod(CryMT_PVolSampler, float4((p + 0.5) / 1024, 256.5 / 513, 0, 0)).x,\n                        tex2Dlod(CryMT_PVolSampler, float4((p + 1.5) / 1024, 256.5 / 513, 0, 0)).x,\n                        tex2Dlod(CryMT_PVolSampler, float4((p + 2.5) / 1024, 256.5 / 513, 0, 0)).x);\n  float3 dims = float3(64, 64, 32);\n  float3 g = (pos - first) / cell - 0.5; // cell coordinates, the cells\' centres at whole numbers\n  float edge = min(min(min(g.x, dims.x - 1 - g.x), min(g.y, dims.y - 1 - g.y)), min(g.z, dims.z - 1 - g.z));\n  if (edge < 0)\n    return;\n  float z0 = floor(g.z);\n  float z1 = min(z0 + 1, dims.z - 1);\n  float fz = g.z - z0;\n  float2 xy = clamp(g.xy + 0.5, 0.5, dims.xy - 0.5);\n  float2 uv0 = (float2(x0, 0) + float2(fmod(z0, 8), floor(z0 / 8)) * dims.xy + xy) / float2(1024, 513);\n  float2 uv1 = (float2(x0, 0) + float2(fmod(z1, 8), floor(z1 / 8)) * dims.xy + xy) / float2(1024, 513);\n  float2 down = float2(0, 257.0 / 513);\n  v = lerp(tex2Dlod(CryMT_PVolSampler, float4(uv0, 0, 0)).xyz, tex2Dlod(CryMT_PVolSampler, float4(uv1, 0, 0)).xyz, fz);\n  tint = lerp(tex2Dlod(CryMT_PVolSampler, float4(uv0 + down, 0, 0)).xyz, tex2Dlod(CryMT_PVolSampler, float4(uv1 + down, 0, 0)).xyz, fz);\n  weight = saturate(edge / 4);\n}\n\nCryMT_PVol CryMT_PVolAt(float3 posFromCamera)\n{\n  float3 fv, ft, cv, ct;\n  float fw, cw;\n  CryMT_PVolOne(posFromCamera, 0, 0, fv, ft, fw);\n  CryMT_PVolOne(posFromCamera, 4, 512, cv, ct, cw);\n  CryMT_PVol r;\n  r.share = lerp(saturate(cv.x) * cw, saturate(fv.x), fw);\n  r.tint = clamp(lerp(lerp(1, ct, cw), ft, fw), 0, 2);\n  float cs = saturate(cv.z) * cw, fs = saturate(fv.z) * fw;\n  r.sun = fs > 0 ? saturate(fv.y) : saturate(cv.y);\n  r.sunWeight = (fv.y < -0.5 || cv.y < -0.5) ? 0 : max(fs, cs); // no sun this frame: the game\'s own\n  return r;\n}\n\n// Thin particles (their own opacity under 0.5: light haze, mist) lose less (down to 35% of it for a clear one): darkened\n// they matched the background and vanished; dense smoke and dust take it all.\nhalf3 CryMT_GIParticleAmbient(half3 ambient, CryMT_PVol vol, half opacity)\n{\n  return ambient * (1 - vol.share * lerp(0.35, 1, saturate(opacity * 2))) * vol.tint;\n}\n\nhalf CryMT_GIParticleSun(half engine, CryMT_PVol vol)\n{\n  return lerp(engine, vol.sun, vol.sunWeight);\n}\n#else\nCryMT_PVol CryMT_PVolAt(float3 posFromCamera)\n{\n  CryMT_PVol r;\n  r.share = 0;\n  r.tint = 1;\n  r.sun = 1;\n  r.sunWeight = 0;\n  return r;\n}\n\nhalf3 CryMT_GIParticleAmbient(half3 ambient, CryMT_PVol vol, half opacity)\n{\n  return ambient;\n}\n\nhalf CryMT_GIParticleSun(half engine, CryMT_PVol vol)\n{\n  return engine;\n}\n#endif\n\npixout CryMT_ParticlePS(v2f IN)\n'),
        ('  pixout OUT;\n  int nQuality = GetShaderQuality();\n',
         '  pixout OUT;\n  int nQuality = GetShaderQuality();\n  CryMT_PVol cryVol = CryMT_PVolAt(IN.vView.xyz); // CryMT: the GI volumes at this particle\n'),
        ('  #if %_RT_AMBIENT\n    cDiffAcc.xyz += Ambient.xyz;\n    finalColor.xyz += Ambient.xyz;      \n  #endif\n',
         '  #if %_RT_AMBIENT\n    half3 cryAmbient = Ambient.xyz;\n  #if %_RT_ALPHABLEND\n    cryAmbient = CryMT_GIParticleAmbient(cryAmbient, cryVol, IN.Color.a); // CryMT GI: smoke and dust\n  #endif\n    cDiffAcc.xyz += cryAmbient;\n    finalColor.xyz += cryAmbient;\n  #endif\n'),
        ('      // Shadow occlusion\n      half fShadow = saturate( 1-dot(shadowOccl, ShadowChanMask) );\n',
         "      // Shadow occlusion\n      half fShadow = saturate( 1-dot(shadowOccl, ShadowChanMask) );\n    #if %_RT_ALPHABLEND\n      if (nType == LT_DIRECTIONAL)\n        fShadow = CryMT_GIParticleSun(fShadow, cryVol); // CryMT: the sun's visibility at the particle\n    #endif\n"),
        ('    PixelShader = compile ps_Auto ParticlePS() GeneralPS;    \n',
         '    PixelShader = compile ps_Auto CryMT_ParticlePS() GeneralPS;    \n'),
        ('    PixelShader = compile ps_Auto ParticlePS() GeneralPS; \n',
         '    PixelShader = compile ps_Auto CryMT_ParticlePS() GeneralPS; \n'),
    ],
    # The z-prepass marks alpha-tested surfaces in a second target (the foliage mask; Settings.cpp InstallGi).
    'shaders/hwscripts/cryfx/commonzpass.cfi': [
        ('pixout Common_ZPassPS(vert2fragZ IN)\n{\n\tpixout OUT = (pixout)0;\n',
         "// CryMT: the z-prepass also marks alpha-tested surfaces (leaves, grass, nets) in a second target that CryMT's back\n// end binds (without it the output goes nowhere): contact shadows and GTAO leave them out as occluders.\n#if D3D10\nstruct pixout_cryzpass\n{\n  float4 Color : COLOR0;\n  float4 Foliage : COLOR1;\n};\n#else\nstruct pixout_cryzpass\n{\n  float4 Color : COLOR0;\n};\n#endif\n\npixout_cryzpass CryMT_ZPassPS(vert2fragZ IN)\n{\n\tpixout_cryzpass OUT = (pixout_cryzpass)0;\n"),
        ('\t\t#if %_RT_ALPHATEST\n\t\t\tfloat baseColor = tex2D(diffuseMapSampler, IN.baseTC.xy).w;\n', '\t\t#if %_RT_ALPHATEST\n\t\t\tfloat baseColor = tex2D(diffuseMapSampler, IN.baseTC.xy).w;\n#if D3D10\n\t\t\tOUT.Foliage = 1; // CryMT\n#endif\n'),
        ('//////////////////////////////// technique ////////////////\n\ntechnique ZPass',
         "// The engine's own z-prepass pixel shader (Methagen.cfx names it): CryMT_ZPassPS without the foliage mask. The ZPass\n// technique below uses CryMT_ZPassPS: the precompiled shared cache (ShaderCache.pak) holds Common_ZPassPS by name\n// and kept the unpatched one.\npixout Common_ZPassPS(vert2fragZ IN)\n{\n  pixout OUT;\n  OUT.Color = CryMT_ZPassPS(IN).Color;\n  return OUT;\n}\n\n//////////////////////////////// technique ////////////////\n\ntechnique ZPass"),
        ('compile ps_3_0 Common_ZPassPS() ZPS;',
         'compile ps_3_0 CryMT_ZPassPS() ZPS;'),
        ('compile ps_Auto Common_ZPassPS() ZPS;',
         'compile ps_Auto CryMT_ZPassPS() ZPS;'),
    ],
    # The deferred shadows rebuild a pixel's point where the jittered depth's point lies (Settings.cpp InstallGi).
    'shaders/hwscripts/cryfx/shadowmaskgen.cfx': [
        ('float4 vBasisMagnitudes\t: PB_TempData[14];\n', 'float4 vBasisMagnitudes\t: PB_TempData[14];\n' + JITTER_DECL),
        ('    P0 = CalcHomogeneousPos(SceneDepth, IN.WPos.xy);',
         '    P0 = CalcHomogeneousPos(SceneDepth, IN.WPos.xy - CryMT_JitterPixels(IN.ScreenTC.xy)); // CryMT: the DLAA jitter'),
        ('  fInShadow = (1-fInShadow) * saturate(fFadeF); //TODO: remove saturate\n',
         '  fInShadow = (1-fInShadow) * saturate(fFadeF); //TODO: remove saturate\n  #if !%_RT_VARIANCE_SM\n    fInShadow = max(fInShadow, CryMT_ContactShadow(IN.WS_ViewVect, SceneDepth, IN.ScreenTC.xy, IN.WPos.xy)); // CryMT: contact shadows\n  #endif\n'),
    ],
}


def placeholder_dds():
    """4x1 A8R8G8B8, all zero: adds nothing and replaces nothing when CryMT's back end does not stand in for it."""
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, 1, 4, 16, 0, 1)
    pixel_format = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return header + pixel_format + caps + bytes(16)


def jitter_dds():
    """8x1 A8R8G8B8: texels 0..6 zero (no jitter, no contact shadows without CryMT's back end), the last 5A5A5A5A (its mark)."""
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, 1, 8, 32, 0, 1)
    pixel_format = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return header + pixel_format + caps + bytes(28) + bytes([0x5A] * 4)


def foliage_dds():
    """2x1 A8R8G8B8: the first texel zero, the second A5A5A5A5 (the foliage mask placeholder's mark)."""
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x100F, 1, 2, 8, 0, 1)
    pixel_format = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return header + pixel_format + caps + bytes(4) + bytes([0xA5] * 4)


def pvol_dds():
    """2x1 A8R8G8B8: the first texel zero, the second 3C3C3C3C (the particles' occlusion volume placeholder's mark)."""
    return foliage_dds()[:132] + bytes([0x3C] * 4)


def read(z, i):
    try:
        return z.read(i)
    except zipfile.BadZipFile:
        i.orig_filename = i.orig_filename.replace('/', chr(92))
        return z.read(i)


pak, out = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(pak)
for i in z.infolist():
    key = i.filename.lower().replace(chr(92), '/')
    if key not in PATCHES:
        continue
    text = read(z, i).decode('latin-1').replace('\r\n', '\n')
    for old, new in PATCHES[key]:
        if text.count(old) != 1:
            sys.exit(f'{key}: expected text not found once: {old[:40]!r}')
        text = text.replace(old, new)
    path = os.path.join(out, *i.filename.replace(chr(92), '/').split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'w', encoding='latin-1', newline='\r\n').write(text)
    print('patched', path)
dds = os.path.join(out, 'Textures', 'CryMT', 'gi.dds')
os.makedirs(os.path.dirname(dds), exist_ok=True)
data = placeholder_dds()
assert len(data) == 128 + 16
open(dds, 'wb').write(data)
print('placeholder', dds, len(data), 'bytes')
jdds = os.path.join(out, 'Textures', 'CryMT', 'jitter.dds')
open(jdds, 'wb').write(jitter_dds())
print('placeholder', jdds, len(jitter_dds()), 'bytes')
fdds = os.path.join(out, 'Textures', 'CryMT', 'foliage.dds')
open(fdds, 'wb').write(foliage_dds())
print('placeholder', fdds, len(foliage_dds()), 'bytes')
vdds = os.path.join(out, 'Textures', 'CryMT', 'pvol.dds')
open(vdds, 'wb').write(pvol_dds())
print('placeholder', vdds, len(pvol_dds()), 'bytes')
