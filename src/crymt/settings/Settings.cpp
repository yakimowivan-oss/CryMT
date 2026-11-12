// CryMTSettings.exe: CryMT's settings window (CryMT\CryMT.ini, read by Bin64\dbghelp.dll at every start of
// Bin64\Crysis.exe). Mode, borderless fullscreen, pause on focus loss, GI with its knobs; installs and removes the GI
// shader patch (fragLib.cfi and Terrain.cfx from the game's own Shaders.pak, edited; CryMT's all-zero placeholder
// Textures\CryMT\gi.dds) and returns the shader cache to its backup when GI is turned off. Can start the game.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
enum
{
    kModeOff = 100, kModeMt, kModeDx12, kModeDlaa,
    kBorderless = 200, kPause, kGi,
    kRadius = 300, kBounce, kAo, kTemporal,
    kSave = 400, kSaveStart, kReports, kClose,
    kStatus = 500,
};

std::wstring g_dir, g_game, g_ini;
bool g_ru;
HWND g_wnd;
HFONT g_font;

const wchar_t* T(const wchar_t* ru, const wchar_t* en) { return g_ru ? ru : en; }

std::wstring ReadIni(const wchar_t* section, const wchar_t* key, const wchar_t* def)
{
    wchar_t v[64] = {};
    GetPrivateProfileStringW(section, key, def, v, 64, g_ini.c_str());
    return v;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ReadAll(const std::wstring& path)
{
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f)
        return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

bool WriteAll(const std::wstring& path, const std::string& data)
{
    SHCreateDirectoryExW(nullptr, path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f)
        return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

bool Run(std::wstring cmd)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// ---- GI shader patch (same edits as tools\prof\patchshaders.py) ---------------------------------------------------
const char* const kEnvAll = R"(#if D3D10
#if !%TEMP_VEGETATION
#if !%TEMP_TERRAIN
  // CryMT: an environment reflection where the material has none (CryMT_EnvSampler; black without CRYMT_ENVMAP).
  if (pPass.nReflectionMapping == 0 && pPass.nQuality != QUALITY_LOW)
  {
    half crymtF = 1 - saturate(pPass.fNdotE);
    crymtF *= crymtF;
    crymtF *= crymtF;
    pPass.cEnvironment = texCUBE(CryMT_EnvSampler, pPass.vReflVec.xyz).rgb * (0.04 + 0.96 * crymtF) * 0.35;
    pPass.nReflectionMapping = 3;
  }
#endif
#endif
#endif

  // do custom pass setup
  frag_custom_begin(pPass);)";

const char* const kGiDecl = R"(#include "CommonRAM.cfi"

// CryMT GI: the engine binds the placeholder Textures/CryMT/gi.dds (all zero: the ambient stays as it was); CryMT's
// D3D12 back end shows its GI texture in its place. rgb = indirect light added, a = share of the engine's ambient
// replaced (sky occlusion) plus flags (below).
#if D3D10
// CryMT environment reflection: the placeholder Textures/CryMT/env.dds (a black cube) where CryMT's D3D12 back end
// shows its environment cube (CRYMT_ENVMAP).
samplerCUBE CryMT_EnvSampler = sampler_state
{
  Texture = Textures/CryMT/env.dds;
  MinFilter = LINEAR;
  MagFilter = LINEAR;
  MipFilter = POINT;
  AddressU = Clamp;
  AddressV = Clamp;
};

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
// and walls, so metal glinted in shadowed rooms. Off for now (the user, 2026-11-09: comparing the reflections without it);
// it was: return 1 - CryMT_GIShare(CryMT_GI(screenProj).w);
half CryMT_GISpecular(float4 screenProj)
{
  return 1;
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

// How much of a baked term the GI hands back at this distance: none up to the distance in the top-left texel (64 x half
// of it, metres), all from 2.5 x that on (the GI fades out over the same window). 0: the GI replaces it everywhere.
half CryMT_GIBakedShare(float4 screenProj)
{
  float start = floor(CryMT_GIControl() / 64) * 2;
  return start > 0 ? saturate((screenProj.w - start) / (1.5 * start)) : 0;
}

// The engine's TerrainAO (under the trees; a 2.5D map: one canopy height per terrain cell): near the camera the GI's
// own occlusion takes its place (bit 8), further the engine's (far trees are sprites, beyond the voxels).
half CryMT_GITerrainAOMix(float4 screenProj, half tao)
{
  if (!CryMT_GIReplaces(screenProj, 8))
    return tao;
  return lerp(1, tao, CryMT_GIBakedShare(screenProj));
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
  return CryMT_GIReplaces(screenProj, bit) ? lerp(1, baked, CryMT_GIBakedShare(screenProj)) : baked;
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
)";

// CryMT's frame texture for the deferred shadows (CryMT's back end binds it in place of the placeholder, whose texels
// read as zero): the camera jitter (the depth at a pixel is the point the unjittered camera sees at the pixel minus
// the jitter) and the sun and camera for the contact shadows.
const char* const kJitterDecl = R"(
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
)";

// Forward+ test (CryMT's D3D12 back end with CRYMT_FPLUS=1 or the engine page's Num1; needs r_LightsSinglePass 1, the
// single-pass light loop): the loop also takes the frame's point lamps without shadow maps from CryMT's lists per
// 32-pixel screen tile, and drops the engine's own copies of them (the same position and radius). The placeholder
// (Textures/CryMT/fplus.dds, texel 0 zero) and cameras other than the frame's (reflections) keep the engine's lights.
// CryMT's 1024 x 128 texture: texel 0 (on, tiles across, down, lamps); 1 + 2k and 2 + 2k lamp k's position from the
// camera and 1 / radius, its colour (a the specular multiplier); 65 the camera; from 128 one per tile (its first index
// texel, its lamp count); then the tiles' lamp indices, 4 a texel.
const char* const kFPlusDecl = R"(
struct CryMT_FPlusTile
{
  int count;   // the tile's lamps (0: off)
  float start; // its first index texel
};

#if D3D10
// The engine's light count for the loop (LightsNum_DB, an int): under D3D10 the engine writes it as a float, so the int
// read the float's bits (3.0 = 1077936128 iterations a pixel: the GPU never finished the frame, the game hung on its
// first frame with r_ShadersDynamicBranching 1). A count that is not small is taken as a float's bits, and the result
// is held to 0..32 (the level's light list holds 32): the user's mine run still hung the GPU in its second loop (frame
// 2207) with only the float case handled, so some draws get other values.
int CryMT_LightsNum(int n)
{
  float f = (n < 0 || n > 64) ? asfloat(n) : (float)n;
  return (int)clamp(f, 0, 32);
}

sampler2D CryMT_FPlusSampler = sampler_state
{
  Texture = Textures/CryMT/fplus.dds;
  MinFilter = POINT;
  MagFilter = POINT;
  MipFilter = POINT;
  AddressU = Clamp;
  AddressV = Clamp;
};

float4 CryMT_FPlusTexel(float t)
{
  float row = floor(t / 1024);
  return tex2Dlod(CryMT_FPlusSampler, float4((t - row * 1024 + 0.5) / 1024, (row + 0.5) / 128, 0, 0));
}

CryMT_FPlusTile CryMT_FPlusAt(float4 screenProj)
{
  CryMT_FPlusTile t;
  t.count = 0;
  t.start = 0;
  float4 h = CryMT_FPlusTexel(0);
  float4 cam = CryMT_FPlusTexel(65);
  if (h.x > 0.5 && all(abs(cam.xyz - PS_WorldViewPos.xyz) < 0.05))
  {
    float2 tile = min(floor(saturate(screenProj.xy / screenProj.w) * h.yz), h.yz - 1);
    float4 g = CryMT_FPlusTexel(128 + tile.y * h.y + tile.x);
    t.count = (int)g.y;
    t.start = g.x;
  }
  return t;
}

void CryMT_FPlusLamp(CryMT_FPlusTile t, float j, out float4 pos, out float4 colour)
{
  float4 ix = CryMT_FPlusTexel(t.start + floor(j / 4));
  float m = j - 4 * floor(j / 4);
  float k = m < 0.5 ? ix.x : m < 1.5 ? ix.y : m < 2.5 ? ix.z : ix.w;
  pos = CryMT_FPlusTexel(1 + 2 * k);
  colour = CryMT_FPlusTexel(2 + 2 * k);
}

bool CryMT_FPlusHas(CryMT_FPlusTile t, float4 lightPos)
{
  bool found = false;
  [unroll]
  for (int j = 0; j < 4; j++)
  {
    float4 pos, colour;
    CryMT_FPlusLamp(t, j, pos, colour);
    if (j < t.count && all(abs(pos.xyz - lightPos.xyz) < 0.25) && abs(pos.w - lightPos.w) <= 0.01 * lightPos.w)
      found = true;
  }
  return found;
}
#else
int CryMT_LightsNum(int n)
{
  return n;
}

CryMT_FPlusTile CryMT_FPlusAt(float4 screenProj)
{
  CryMT_FPlusTile t;
  t.count = 0;
  t.start = 0;
  return t;
}

void CryMT_FPlusLamp(CryMT_FPlusTile t, float j, out float4 pos, out float4 colour)
{
  pos = 0;
  colour = 0;
}

bool CryMT_FPlusHas(CryMT_FPlusTile t, float4 lightPos)
{
  return false;
}
#endif
)";

// The single-pass light loop with 4 slots for CryMT's tile lamps after the engine's (kFPlusDecl). The loop's count is
// the engine's constant plus 4, the same for every pixel: a count read from the tile texture varies per pixel, and the
// gradient samples inside the loop (parallax, terrain layers' per-light code) then failed to compile under D3D10 (X3570),
// so those passes were not drawn (the user: flat, one-colour textures almost everywhere: terrain detail layers gone).
const char* const kFPlusLoop = R"(#if %DYN_BRANCHING
  float2 tcLI = LightInfoTC_DB.xy;
  float tcIter = LightInfoTC_DB.z;
  float tcIterParam = LightInfoTC_DB.w;
  CryMT_FPlusTile cryTile = CryMT_FPlusAt(pPass.IN.screenProj); // CryMT Forward+: this screen tile's lamps
  int cryLightsDB = CryMT_LightsNum(LightsNum_DB);
  for (int i=0; i<cryLightsDB+4; i++, tcLI.x+=tcIter)
#else
)";

const char* const kFPlusLight = R"(#if %DYN_BRANCHING
    // We can't index constants in cycle, so get light/shadow info from the texture
    half4 Diffuse;
    half4 ShadowChanMask;
    float nType;
    if (i < cryLightsDB)
    {
      WorldLightPos = tex2Dlod(LightInfoSampler_DB, float4(tcLI, 0, 0));
      Diffuse = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam, tcLI.y, 0, 0));
      ShadowChanMask = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam*3, tcLI.y, 0, 0));
      nType = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam*2, tcLI.y, 0, 0)).w;
      // CryMT Forward+: the engine's copy of a lamp the tile gives goes (no light, no falloff)
      if (cryTile.count > 0 && nType == LT_OMNI && CryMT_FPlusHas(cryTile, WorldLightPos))
      {
        WorldLightPos.w = 1e6;
        Diffuse = 0;
      }
    }
    else
    {
      float4 cryColour;
      CryMT_FPlusLamp(cryTile, i - cryLightsDB, WorldLightPos, cryColour); // CryMT Forward+: the tile's lamp
      Diffuse = cryColour;
      if (i - cryLightsDB >= cryTile.count)
      {
        WorldLightPos.w = 1e6; // an empty slot: no light, no falloff
        Diffuse = 0;
      }
      ShadowChanMask = 0;
      nType = LT_OMNI;
    }
    half4 Specular = Diffuse;
    Specular.xyz *= Diffuse.w;
#else
)";

const char* const kGiAmbient = R"(  // custom ambient pass
  frag_custom_ambient(pPass, amb);

#if !%_RT_FOG
  pPass.cAmbientAcc.xyz = CryMT_GIAmbient(pPass.cAmbientAcc.xyz, pPass.IN.screenProj);
#endif)";

bool Replace(std::string& text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}

std::string Lf(std::string s)
{
    std::string out;
    for (char c : s)
        if (c != '\r')
            out += c;
    return out;
}

std::string Crlf(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

const wchar_t* kFragLib = L"Shaders\\HWScripts\\CryFX\\fragLib.cfi";
const wchar_t* kTerrain = L"Shaders\\HWScripts\\CryFX\\Terrain.cfx";
const wchar_t* kShadowMask = L"Shaders\\HWScripts\\CryFX\\ShadowMaskGen.cfx";
const wchar_t* kZPass = L"Shaders\\HWScripts\\CryFX\\CommonZPass.cfi";
const wchar_t* kParticles = L"Shaders\\HWScripts\\CryFX\\Particles.cfi";
const wchar_t* kMetal = L"Shaders\\HWScripts\\CryFX\\Metal.cfx";
const wchar_t* kVegetation = L"Shaders\\HWScripts\\CryFX\\Vegetation.cfx";
const wchar_t* kPlaceholder = L"Textures\\CryMT\\gi.dds";
// The patch goes into the game as a pak: without -DEVMODE CryPak reads files only from paks (loose files are ignored)
// and a later pak by name overrides the earlier ones (as the official ZPatch1.pak does), so zzCryMT.pak's sources
// replace Shaders.pak's; the engine compiles the changed shaders into the user shader cache on first use.
const wchar_t* kPak = L"zzCryMT.pak";

// A zip with stored entries and no data descriptors, directory entries or extra fields: CryPak took this one (a
// tar.exe zip, deflated with data descriptors, it ignored).
bool WriteStoredZip(const std::wstring& path, const std::vector<std::pair<std::string, std::string>>& entries)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const auto crc32 = [](const std::string& d) {
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char b : d)
            c = table[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    // A pak that already holds exactly these entries is left as it is: rewriting it gives them new dates, and the engine
    // then parses the shader sources again (the first start after that draws objects grey while they compile). The
    // launcher installs before every start, so this keeps a start without changes a start without shader work.
    const std::string old = ReadAll(path);
    if (!old.empty())
    {
        const auto rd16 = [&](size_t at) { return static_cast<uint32_t>(static_cast<unsigned char>(old[at]) | static_cast<unsigned char>(old[at + 1]) << 8); };
        const auto rd32 = [&](size_t at) { return rd16(at) | rd16(at + 2) << 16; };
        size_t at = 0;
        bool same = true;
        for (const auto& [name, data] : entries)
        {
            if (at + 30 > old.size() || rd32(at) != 0x04034B50 || rd16(at + 8) != 0)
            {
                same = false;
                break;
            }
            const size_t nameLen = rd16(at + 26), extraLen = rd16(at + 28), size = rd32(at + 18), body = at + 30 + nameLen + extraLen;
            if (nameLen != name.size() || size != data.size() || body + size > old.size() || old.compare(at + 30, nameLen, name) != 0 ||
                old.compare(body, size, data) != 0)
            {
                same = false;
                break;
            }
            at = body + size;
        }
        if (same && at + 4 <= old.size() && rd32(at) == 0x02014B50)
            return true;
    }
    std::string out, central;
    // The entries' time and date: now (the engine compares a source's date with its parsed cache, Shaders/Cache/*.cfib;
    // a fixed date kept the first GI version's fragLib parsed after the patch changed).
    SYSTEMTIME now;
    GetLocalTime(&now);
    const uint32_t dosTime = (now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2);
    const uint32_t dosDate = ((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay;
    const auto u16 = [](std::string& o, uint32_t v) { o.push_back(static_cast<char>(v)); o.push_back(static_cast<char>(v >> 8)); };
    const auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
    for (const auto& [name, data] : entries)
    {
        const uint32_t crc = crc32(data), size = static_cast<uint32_t>(data.size()), offset = static_cast<uint32_t>(out.size());
        u32(out, 0x04034B50); u16(out, 10); u16(out, 0); u16(out, 0); u16(out, dosTime); u16(out, dosDate);
        u32(out, crc); u32(out, size); u32(out, size); u16(out, static_cast<uint32_t>(name.size())); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014B50); u16(central, 20); u16(central, 10); u16(central, 0); u16(central, 0); u16(central, dosTime);
        u16(central, dosDate); u32(central, crc); u32(central, size); u32(central, size); u16(central, static_cast<uint32_t>(name.size()));
        u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
        central += name;
    }
    const uint32_t centralAt = static_cast<uint32_t>(out.size()), count = static_cast<uint32_t>(entries.size());
    out += central;
    u32(out, 0x06054B50); u16(out, 0); u16(out, 0); u16(out, count); u16(out, count);
    u32(out, static_cast<uint32_t>(central.size())); u32(out, centralAt); u16(out, 0);
    return WriteAll(path, out);
}

std::wstring UserShaders()
{
    wchar_t docs[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs);
    return std::wstring(docs) + L"\\My Games\\Crysis\\Shaders";
}

void CopyTree(const std::wstring& from, const std::wstring& to)
{
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            CopyTree(from + L"\\" + name, to + L"\\" + name);
        else
            CopyFileW((from + L"\\" + name).c_str(), (to + L"\\" + name).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Files compiled from the patched shaders go; the rest comes back from the backup.
void RestoreTree(const std::wstring& cur, const std::wstring& backup)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((cur + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring c = cur + L"\\" + name, b = backup + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            RestoreTree(c, b);
        else if (!Exists(b))
            DeleteFileW(c.c_str());
        else if (ReadAll(c) != ReadAll(b))
            CopyFileW(b.c_str(), c.c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring InstallGi()
{
    const std::wstring backup = g_dir + L"cache\\usershaders_backup";
    if (!Exists(backup) && Exists(UserShaders()))
        CopyTree(UserShaders(), backup);
    const std::wstring tmp = g_dir + L"cache\\shaders_src";
    SHCreateDirectoryExW(nullptr, tmp.c_str(), nullptr);
    if (!Run(L"tar.exe -xf \"" + g_game + L"Game\\Shaders.pak\" -C \"" + tmp + L"\" Shaders/HWScripts/CryFX/fragLib.cfi Shaders/HWScripts/CryFX/Terrain.cfx Shaders/HWScripts/CryFX/ShadowMaskGen.cfx Shaders/HWScripts/CryFX/CommonZPass.cfi Shaders/HWScripts/CryFX/Particles.cfi Shaders/HWScripts/CryFX/Metal.cfx Shaders/HWScripts/CryFX/Vegetation.cfx"))
        return T(L"не удалось распаковать Shaders.pak", L"could not unpack Shaders.pak");
    std::string frag = Lf(ReadAll(tmp + L"\\" + kFragLib)), terrain = Lf(ReadAll(tmp + L"\\" + kTerrain)),
                shadowMask = Lf(ReadAll(tmp + L"\\" + kShadowMask)), zpass = Lf(ReadAll(tmp + L"\\" + kZPass)),
                particles = Lf(ReadAll(tmp + L"\\" + kParticles)), metal = Lf(ReadAll(tmp + L"\\" + kMetal)),
                vegetation = Lf(ReadAll(tmp + L"\\" + kVegetation));
    wchar_t fpv[8] = {};
    const bool fplusLoop = !(GetEnvironmentVariableW(L"CRYMT_FPLUS_SHADERS", fpv, 8) && fpv[0] == L'0');
    const std::string dbLoop = "#if %DYN_BRANCHING\n  float2 tcLI = LightInfoTC_DB.xy;\n  float tcIter = LightInfoTC_DB.z;\n"
                               "  float tcIterParam = LightInfoTC_DB.w;\n  for (int i=0; i<LightsNum_DB; i++, tcLI.x+=tcIter)\n#else\n";
    const bool ok = Replace(frag, "#include \"CommonRAM.cfi\"\n", std::string(kGiDecl) + kFPlusDecl) &&
                    // The single-pass loop's light count is a float's bits under D3D10 (CryMT_LightsNum): vegetation's
                    // back shading divides by it (before fragLib is included, so the conversion is written out here).
                    Replace(vegetation, "  if (LightsNum_DB)\n    fOccFactor = pPass.pCustom.fOcclShadowAcc / LightsNum_DB;\n",
                            "  if (LightsNum_DB)\n  #if D3D10\n    fOccFactor = saturate(pPass.pCustom.fOcclShadowAcc / max((LightsNum_DB < 0 || LightsNum_DB > 64) ? "
                            "asfloat(LightsNum_DB) : (float)LightsNum_DB, 1.0)); // CryMT: the count is a float's bits under D3D10\n"
                            "  #else\n    fOccFactor = pPass.pCustom.fOcclShadowAcc / LightsNum_DB;\n  #endif\n") &&
                    // Forward+ test: the single-pass light loop also takes CryMT's tile lamps (kFPlusDecl). With
                    // CRYMT_FPLUS_SHADERS=0 (tests) only the loop's light count is fixed.
                    (fplusLoop ? Replace(frag, dbLoop, kFPlusLoop) &&
                                     Replace(frag, "#if %DYN_BRANCHING\n    // We can't index constants in cycle, so get light/shadow info from the texture\n"
                                                   "    WorldLightPos = tex2Dlod(LightInfoSampler_DB, float4(tcLI, 0, 0));\n"
                                                   "    half4 Diffuse = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam, tcLI.y, 0, 0));\n"
                                                   "    half4 Specular = Diffuse;\n    Specular.xyz *= Diffuse.w;\n"
                                                   "    half4 ShadowChanMask = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam*3, tcLI.y, 0, 0));\n"
                                                   "    float nType = tex2Dlod(LightInfoSampler_DB, float4(tcLI.x+tcIterParam*2, tcLI.y, 0, 0)).w;\n#else\n",
                                             kFPlusLight)
                               : Replace(frag, dbLoop,
                                         "#if %DYN_BRANCHING\n  float2 tcLI = LightInfoTC_DB.xy;\n  float tcIter = LightInfoTC_DB.z;\n"
                                         "  float tcIterParam = LightInfoTC_DB.w;\n  for (int i=0; i<CryMT_LightsNum(LightsNum_DB); i++, tcLI.x+=tcIter) // CryMT: the count\n#else\n")) &&
                    // The environment reflection on materials without their own (not vegetation or terrain): composed as
                    // the engine composes its own (shaded by the light reaching the surface, times the gloss map and the
                    // specular colour), Schlick's fresnel; Metal stronger (it defines CRYMT_ENV_STRENGTH).
                    Replace(frag, "  // do custom pass setup\n  frag_custom_begin(pPass);", kEnvAll) &&
                    Replace(metal, "void frag_custom_begin(inout fragPass pPass)\n{\n",
                            "void frag_custom_begin(inout fragPass pPass)\n{\n  // CryMT: Metal's environment reflection at 1.0 (fragLib's is 0.35; no preprocessor: CryFX failed on it)\n"
                            "  if (pPass.nReflectionMapping == 3)\n    pPass.cEnvironment *= 2.85;\n") &&
                    Replace(frag, "  // custom ambient pass\n  frag_custom_ambient(pPass, amb);", kGiAmbient) &&
                    Replace(frag, "  half3 cSpecular = pPass.cSpecularAcc.xyz * pPass.cGlossMap.xyz;                    // 2 alu",
                            "  half3 cSpecular = pPass.cSpecularAcc.xyz * pPass.cGlossMap.xyz;                    // 2 alu\n#if !%_RT_FOG\n  cSpecular *= CryMT_GISpecular(pPass.IN.screenProj); // CryMT GI: specular occlusion (lights and environment map)\n#endif") &&
                    // The baked occlusion (RAM, sky accessibility) and the SSAO term mixed by the GI's flags.
                    Replace(frag, "    amb.xyz\t*=\tpPass.cBumpMap.z*RamTex;", "    amb.xyz\t*=\tCryMT_GIBakedMix(pPass.IN.screenProj, pPass.cBumpMap.z*RamTex, 2);") &&
                    Replace(frag, "    amb.xyz *= GetSkyAmount(", "    amb.xyz *= CryMT_GIBakedMix(pPass.IN.screenProj, GetSkyAmount(") &&
                    Replace(frag, "    pPass.IN.screenProj.xyzw);\n #else", "    pPass.IN.screenProj.xyzw), 4);\n #else") &&
                    Replace(frag, "    pPass.cAOMapVal = vAO.x*vAO.y*vAO.w;",
                            "  #if %GRASS\n    pPass.cAOMapVal = vAO.x * CryMT_GITerrainAOMix(pPass.IN.screenProj, vAO.y) * vAO.w; // CryMT GI: grass keeps the engine's SSAO\n  #else\n    pPass.cAOMapVal = CryMT_GISSAOMix(pPass.IN.screenProj, vAO.x) * CryMT_GITerrainAOMix(pPass.IN.screenProj, vAO.y) * vAO.w; // CryMT GI\n  #endif") &&
                    Replace(terrain, "AmbientOp.xyz * IN.Color.a * (vAO.x * vAO.y * vAO.w)); // ambient",
                            "CryMT_GIAmbient(AmbientOp.xyz * CryMT_GIBakedMix(IN.screenProj, IN.Color.a, 16), IN.screenProj) * "
                            "(CryMT_GISSAOMix(IN.screenProj, vAO.x) * CryMT_GITerrainAOMix(IN.screenProj, vAO.y) * vAO.w)); // ambient (CryMT GI)") &&
                    // The deferred shadows rebuild a pixel's point where the jittered depth's point lies (CryMT_JitterPixels).
                    Replace(shadowMask, "float4 vBasisMagnitudes\t: PB_TempData[14];\n", std::string("float4 vBasisMagnitudes\t: PB_TempData[14];\n") + kJitterDecl) &&
                    Replace(shadowMask, "    P0 = CalcHomogeneousPos(SceneDepth, IN.WPos.xy);",
                            "    P0 = CalcHomogeneousPos(SceneDepth, IN.WPos.xy - CryMT_JitterPixels(IN.ScreenTC.xy)); // CryMT: the DLAA jitter") &&
                    // Blended particles (smoke, dust) take the GI for their ambient.
                    Replace(particles, "pixout ParticlePS(v2f IN)\n",
                            "// CryMT GI for blended particles (smoke, dust), from the D3D12 back end's volumes around the camera (in place of\n// Textures/CryMT/pvol.dds): per cell x the share of ambient a particle there loses (outdoors a part of it, capped), y\n// the sun's visibility through the voxels (-1: no sun), z the indoor share; and the tint of the light the voxels bring\n// there (hue only: luminance 1, part white). The game lit particles by the sun with the screen's shadow mask behind\n// each pixel (and our first try took the GI there too): both flipped as dust crossed lit and shaded backgrounds (a\n// sunlit floor, a dark wall, a window). The voxels' sun is taken indoors (by the indoor share); outdoors the game's (the\n// voxels' grass and coarse ground cells shaded dust in the open sun). Two volumes of 64 x 64 x 32 cells: fine (0.5 m:\n// walls and ceilings of rooms) and coarse (1.5 m, +-48 m), each as z-slices tiled 8 across 512 x 256 of a 1024 x 513\n// atlas (RGBA32F: fine left, coarse right; rows 0-255 the shares, 257-512 the tints); row 256's texels 0-3 and 4-7 their\n// first cell relative to the camera (metres) and cell size. Read once per pixel (CryMT_PVolAt at the shader's start):\n// two slices blended per volume; the fine one where it reaches, over its outer cells to the coarse one, which fades out\n// over its own (then the game's own). The ambient is darkened and tinted, not brightened. Additive particles (fire,\n// sparks, flashes) are left as they are. The placeholder (no CryMT, or the effect off: key \"-\") has cell sizes of 0: the\n// game's own. The pixel shader is renamed so no cached build of the old one is used.\nstruct CryMT_PVol\n{\n  float share;     // of the ambient that goes\n  float3 tint;     // the ambient's tint\n  float sun;       // the sun's visibility (the voxels')\n  float sunWeight; // how much of it to take (indoors, inside the volumes); 0: the game's own\n};\n\n#if D3D10\nsampler2D CryMT_PVolSampler = sampler_state\n{\n  Texture = Textures/CryMT/pvol.dds;\n  MinFilter = LINEAR;\n  MagFilter = LINEAR;\n  MipFilter = POINT;\n  AddressU = Clamp;\n  AddressV = Clamp;\n};\n\n// One volume (params at row 256's texels p .. p + 3, slices from atlas column x0): its share, sun and indoor share, its\n// tint, its weight (0 outside, rising over the outer 4 cells).\nvoid CryMT_PVolOne(float3 pos, float p, float x0, out float3 v, out float3 tint, out float weight)\n{\n  v = 0;\n  tint = 1;\n  weight = 0;\n  float cell = tex2Dlod(CryMT_PVolSampler, float4((p + 3.5) / 1024, 256.5 / 513, 0, 0)).x;\n  if (cell <= 0)\n    return;\n  float3 first = float3(tex2Dlod(CryMT_PVolSampler, float4((p + 0.5) / 1024, 256.5 / 513, 0, 0)).x,\n                        tex2Dlod(CryMT_PVolSampler, float4((p + 1.5) / 1024, 256.5 / 513, 0, 0)).x,\n                        tex2Dlod(CryMT_PVolSampler, float4((p + 2.5) / 1024, 256.5 / 513, 0, 0)).x);\n  float3 dims = float3(64, 64, 32);\n  float3 g = (pos - first) / cell - 0.5; // cell coordinates, the cells' centres at whole numbers\n  float edge = min(min(min(g.x, dims.x - 1 - g.x), min(g.y, dims.y - 1 - g.y)), min(g.z, dims.z - 1 - g.z));\n  if (edge < 0)\n    return;\n  float z0 = floor(g.z);\n  float z1 = min(z0 + 1, dims.z - 1);\n  float fz = g.z - z0;\n  float2 xy = clamp(g.xy + 0.5, 0.5, dims.xy - 0.5);\n  float2 uv0 = (float2(x0, 0) + float2(fmod(z0, 8), floor(z0 / 8)) * dims.xy + xy) / float2(1024, 513);\n  float2 uv1 = (float2(x0, 0) + float2(fmod(z1, 8), floor(z1 / 8)) * dims.xy + xy) / float2(1024, 513);\n  float2 down = float2(0, 257.0 / 513);\n  v = lerp(tex2Dlod(CryMT_PVolSampler, float4(uv0, 0, 0)).xyz, tex2Dlod(CryMT_PVolSampler, float4(uv1, 0, 0)).xyz, fz);\n  tint = lerp(tex2Dlod(CryMT_PVolSampler, float4(uv0 + down, 0, 0)).xyz, tex2Dlod(CryMT_PVolSampler, float4(uv1 + down, 0, 0)).xyz, fz);\n  weight = saturate(edge / 4);\n}\n\nCryMT_PVol CryMT_PVolAt(float3 posFromCamera)\n{\n  float3 fv, ft, cv, ct;\n  float fw, cw;\n  CryMT_PVolOne(posFromCamera, 0, 0, fv, ft, fw);\n  CryMT_PVolOne(posFromCamera, 4, 512, cv, ct, cw);\n  CryMT_PVol r;\n  r.share = lerp(saturate(cv.x) * cw, saturate(fv.x), fw);\n  r.tint = clamp(lerp(lerp(1, ct, cw), ft, fw), 0, 2);\n  float cs = saturate(cv.z) * cw, fs = saturate(fv.z) * fw;\n  r.sun = fs > 0 ? saturate(fv.y) : saturate(cv.y);\n  r.sunWeight = (fv.y < -0.5 || cv.y < -0.5) ? 0 : max(fs, cs); // no sun this frame: the game's own\n  return r;\n}\n\n// Thin particles (their own opacity under 0.5: light haze, mist) lose less (down to 35% of it for a clear one): darkened\n// they matched the background and vanished; dense smoke and dust take it all.\nhalf3 CryMT_GIParticleAmbient(half3 ambient, CryMT_PVol vol, half opacity)\n{\n  return ambient * (1 - vol.share * lerp(0.35, 1, saturate(opacity * 2))) * vol.tint;\n}\n\nhalf CryMT_GIParticleSun(half engine, CryMT_PVol vol)\n{\n  return lerp(engine, vol.sun, vol.sunWeight);\n}\n#else\nCryMT_PVol CryMT_PVolAt(float3 posFromCamera)\n{\n  CryMT_PVol r;\n  r.share = 0;\n  r.tint = 1;\n  r.sun = 1;\n  r.sunWeight = 0;\n  return r;\n}\n\nhalf3 CryMT_GIParticleAmbient(half3 ambient, CryMT_PVol vol, half opacity)\n{\n  return ambient;\n}\n\nhalf CryMT_GIParticleSun(half engine, CryMT_PVol vol)\n{\n  return engine;\n}\n#endif\n\npixout CryMT_ParticlePS(v2f IN)\n") &&
                    Replace(particles, "  pixout OUT;\n  int nQuality = GetShaderQuality();\n",
                            "  pixout OUT;\n  int nQuality = GetShaderQuality();\n  CryMT_PVol cryVol = CryMT_PVolAt(IN.vView.xyz); // CryMT: the GI volumes at this particle\n") &&
                    Replace(particles, "  #if %_RT_AMBIENT\n    cDiffAcc.xyz += Ambient.xyz;\n    finalColor.xyz += Ambient.xyz;      \n  #endif\n",
                            "  #if %_RT_AMBIENT\n    half3 cryAmbient = Ambient.xyz;\n  #if %_RT_ALPHABLEND\n    cryAmbient = CryMT_GIParticleAmbient(cryAmbient, cryVol, IN.Color.a); // CryMT GI: smoke and dust\n  #endif\n    cDiffAcc.xyz += cryAmbient;\n    finalColor.xyz += cryAmbient;\n  #endif\n") &&
                    Replace(particles, "      // Shadow occlusion\n      half fShadow = saturate( 1-dot(shadowOccl, ShadowChanMask) );\n",
                            "      // Shadow occlusion\n      half fShadow = saturate( 1-dot(shadowOccl, ShadowChanMask) );\n    #if %_RT_ALPHABLEND\n      if (nType == LT_DIRECTIONAL)\n        fShadow = CryMT_GIParticleSun(fShadow, cryVol); // CryMT: the sun's visibility at the particle\n    #endif\n") &&
                    Replace(particles, "    PixelShader = compile ps_Auto ParticlePS() GeneralPS;    \n",
                            "    PixelShader = compile ps_Auto CryMT_ParticlePS() GeneralPS;    \n") &&
                    Replace(particles, "    PixelShader = compile ps_Auto ParticlePS() GeneralPS; \n",
                            "    PixelShader = compile ps_Auto CryMT_ParticlePS() GeneralPS; \n") &&
                    // The z-prepass marks alpha-tested surfaces (the foliage mask).
                    Replace(zpass, "pixout Common_ZPassPS(vert2fragZ IN)\n{\n\tpixout OUT = (pixout)0;\n",
                            "// CryMT: the z-prepass also marks alpha-tested surfaces (leaves, grass, nets) in a second target that CryMT's back\n// end binds (without it the output goes nowhere): contact shadows and GTAO leave them out as occluders.\n#if D3D10\nstruct pixout_cryzpass\n{\n  float4 Color : COLOR0;\n  float4 Foliage : COLOR1;\n};\n#else\nstruct pixout_cryzpass\n{\n  float4 Color : COLOR0;\n};\n#endif\n\npixout_cryzpass CryMT_ZPassPS(vert2fragZ IN)\n{\n\tpixout_cryzpass OUT = (pixout_cryzpass)0;\n") &&
                    Replace(zpass, "\t\t#if %_RT_ALPHATEST\n\t\t\tfloat baseColor = tex2D(diffuseMapSampler, IN.baseTC.xy).w;\n",
                            "\t\t#if %_RT_ALPHATEST\n\t\t\tfloat baseColor = tex2D(diffuseMapSampler, IN.baseTC.xy).w;\n#if D3D10\n  #if %GRASS\n\t\t\tOUT.Foliage = 0.6; // CryMT: grass (keeps the engine's SSAO, see fragLib)\n  #else\n\t\t\tOUT.Foliage = 1; // CryMT\n  #endif\n#endif\n") &&
                    Replace(zpass, "//////////////////////////////// technique ////////////////\n\ntechnique ZPass",
                            "// The engine's own z-prepass pixel shader (Methagen.cfx names it): CryMT_ZPassPS without the foliage mask. The ZPass\n// technique below uses CryMT_ZPassPS: the precompiled shared cache (ShaderCache.pak) holds Common_ZPassPS by name\n// and kept the unpatched one.\npixout Common_ZPassPS(vert2fragZ IN)\n{\n  pixout OUT;\n  OUT.Color = CryMT_ZPassPS(IN).Color;\n  return OUT;\n}\n\n//////////////////////////////// technique ////////////////\n\ntechnique ZPass") &&
                    Replace(zpass, "compile ps_3_0 Common_ZPassPS() ZPS;",
                            "compile ps_3_0 CryMT_ZPassPS() ZPS;") &&
                    Replace(zpass, "compile ps_Auto Common_ZPassPS() ZPS;",
                            "compile ps_Auto CryMT_ZPassPS() ZPS;") &&
                    // Contact shadows in the sun's mask (not the terrain's variance pass).
                    Replace(shadowMask, "  fInShadow = (1-fInShadow) * saturate(fFadeF); //TODO: remove saturate\n",
                            "  fInShadow = (1-fInShadow) * saturate(fFadeF); //TODO: remove saturate\n  #if !%_RT_VARIANCE_SM\n    fInShadow = max(fInShadow, CryMT_ContactShadow(IN.WS_ViewVect, SceneDepth, IN.ScreenTC.xy, IN.WPos.xy)); // CryMT: contact shadows\n  #endif\n");
    if (!ok)
        return T(L"шейдеры игры не той версии, патч не применён", L"the game's shaders differ, patch not applied");
    std::string dds(144, '\0');
    const unsigned int header[] = {0x20534444, 124, 0x100F, 1, 4, 16, 0, 1};
    memcpy(&dds[0], header, sizeof(header));
    const unsigned int pf[] = {32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, 0x1000};
    memcpy(&dds[76], pf, sizeof(pf));
    const std::wstring pak = g_game + L"Game\\" + kPak;
    // The frame texture's placeholder: 8x1, texels 0..6 zero (no jitter, no contact shadows without CryMT), the last
    // 5A5A5A5A (the mark).
    std::string jitter(160, '\0');
    const unsigned int jheader[] = {0x20534444, 124, 0x100F, 1, 8, 32, 0, 1};
    memcpy(&jitter[0], jheader, sizeof(jheader));
    memcpy(&jitter[76], pf, sizeof(pf));
    memset(&jitter[156], 0x5A, 4);
    // The foliage mask's placeholder: 2x1, the first texel zero, the second A5A5A5A5 (the mark).
    std::string foliage(136, '\0');
    const unsigned int fheader[] = {0x20534444, 124, 0x100F, 1, 2, 8, 0, 1};
    memcpy(&foliage[0], fheader, sizeof(fheader));
    memcpy(&foliage[76], pf, sizeof(pf));
    memset(&foliage[132], 0xA5, 4);
    // The particles' occlusion volume placeholder: 2x1, the first texel zero (cell size 0: no change), the second 3C3C3C3C.
    std::string pvol = foliage;
    memset(&pvol[132], 0x3C, 4);
    // The Forward+ test's placeholder: 2x1, the first texel zero (off), the second 6B6B6B6B.
    std::string fplus = foliage;
    memset(&fplus[132], 0x6B, 4);
    // The environment reflection's placeholder: a 4x4 cube (A8R8G8B8, 6 faces), black; face 0's second texel E7000000.
    std::string env(128 + 6 * 64, '\0');
    const unsigned int eheader[] = {0x20534444, 124, 0x100F, 4, 4, 16, 0, 1};
    memcpy(&env[0], eheader, sizeof(eheader));
    memcpy(&env[76], pf, sizeof(pf));
    const unsigned int ecaps[] = {0x1008, 0xFE00}; // complex texture; cube map with all six faces
    memcpy(&env[108], ecaps, sizeof(ecaps));
    env[128 + 4 + 3] = static_cast<char>(0xE7);
    if (!WriteStoredZip(pak, {{"Shaders/HWScripts/CryFX/fragLib.cfi", Crlf(frag)},
                              {"Shaders/HWScripts/CryFX/Terrain.cfx", Crlf(terrain)},
                              {"Shaders/HWScripts/CryFX/ShadowMaskGen.cfx", Crlf(shadowMask)},
                              {"Shaders/HWScripts/CryFX/CommonZPass.cfi", Crlf(zpass)},
                              {"Shaders/HWScripts/CryFX/Particles.cfi", Crlf(particles)},
                              {"Shaders/HWScripts/CryFX/Metal.cfx", Crlf(metal)},
                              {"Shaders/HWScripts/CryFX/Vegetation.cfx", Crlf(vegetation)},
                              {"Textures/CryMT/env.dds", env},
                              {"Textures/CryMT/gi.dds", dds},
                              {"Textures/CryMT/jitter.dds", jitter},
                              {"Textures/CryMT/foliage.dds", foliage},
                              {"Textures/CryMT/pvol.dds", pvol},
                              {"Textures/CryMT/fplus.dds", fplus}}))
        return T(L"не удалось записать Game\\zzCryMT.pak", L"could not write Game\\zzCryMT.pak");
    // Loose copies from earlier versions: -DEVMODE would read them before the pak.
    DeleteFileW((g_game + L"Game\\" + kFragLib).c_str());
    DeleteFileW((g_game + L"Game\\" + kTerrain).c_str());
    DeleteFileW((g_game + L"Game\\" + kPlaceholder).c_str());
    RemoveDirectoryW((g_game + L"Game\\Textures\\CryMT").c_str());
    return {};
}

void RemoveGi()
{
    if (!Exists(g_game + L"Game\\" + kPlaceholder) && !Exists(g_game + L"Game\\" + kPak))
        return;
    DeleteFileW((g_game + L"Game\\" + kPak).c_str());
    DeleteFileW((g_game + L"Game\\" + kFragLib).c_str());
    DeleteFileW((g_game + L"Game\\" + kTerrain).c_str());
    DeleteFileW((g_game + L"Game\\" + kPlaceholder).c_str());
    RemoveDirectoryW((g_game + L"Game\\Textures\\CryMT").c_str());
    const std::wstring backup = g_dir + L"cache\\usershaders_backup";
    if (Exists(backup))
        RestoreTree(UserShaders(), backup);
}

// ---- window ---------------------------------------------------------------------------------------------------------
std::wstring Text(int id)
{
    wchar_t t[64] = {};
    GetDlgItemTextW(g_wnd, id, t, 64);
    return t;
}

std::wstring Status()
{
    std::wstring s;
    DWORD dummy = 0;
    const std::wstring exe = g_game + L"Bin64\\Crysis.exe";
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    std::vector<BYTE> info(size);
    wchar_t* product = nullptr;
    wchar_t* version = nullptr;
    UINT len = 0;
    WORD* lang = nullptr;
    if (size && GetFileVersionInfoW(exe.c_str(), 0, size, info.data()) &&
        VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lang), &len) && len >= 4)
    {
        wchar_t key[64];
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName", lang[0], lang[1]);
        VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&product), &len);
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileVersion", lang[0], lang[1]);
        VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&version), &len);
    }
    const bool c1 = product && wcsstr(product, L"C1-Launcher");
    const bool v8 = c1 && version && (wcsstr(version, L"v8") || wcsstr(version, L"v9"));
    s += v8 ? T(L"c1-launcher v8: есть", L"c1-launcher v8: found")
       : c1 ? T(L"c1-launcher старше v8: CryMT ставит своё исправление памяти", L"c1-launcher older than v8: CryMT adds its own memory fix")
            : T(L"Bin64\\Crysis.exe — не c1-launcher: с ним CryMT не проверялся", L"Bin64\\Crysis.exe is not c1-launcher: CryMT is untested with it");
    s += L"\r\n";
    s += Exists(g_game + L"Bin64\\dbghelp.dll") ? T(L"Загрузчик Bin64\\dbghelp.dll: есть", L"Loader Bin64\\dbghelp.dll: found")
                                                 : T(L"Нет Bin64\\dbghelp.dll: распакуйте архив CryMT в папку игры", L"Bin64\\dbghelp.dll missing: unpack CryMT into the game folder");
    const std::string off = ReadAll(g_dir + L"logs\\mt\\crymt_off.txt");
    if (!off.empty())
        s += L"\r\n" + std::wstring(T(L"Прошлый запуск: ", L"Last start: ")) + std::wstring(off.begin(), off.end());
    return s;
}

void Load()
{
    const std::wstring mode = ReadIni(L"CryMT", L"Mode", L"mt");
    CheckRadioButton(g_wnd, kModeOff, kModeDlaa, mode == L"off" ? kModeOff : mode == L"dx12" ? kModeDx12 : mode == L"dlaa" ? kModeDlaa : kModeMt);
    CheckDlgButton(g_wnd, kBorderless, ReadIni(L"CryMT", L"Borderless", L"1") != L"0");
    CheckDlgButton(g_wnd, kPause, ReadIni(L"CryMT", L"PauseOnFocusLoss", L"1") != L"0");
    CheckDlgButton(g_wnd, kGi, ReadIni(L"CryMT", L"GI", L"0") != L"0");
    SetDlgItemTextW(g_wnd, kRadius, ReadIni(L"Env", L"CRYMT_GI_RADIUS", L"2.5").c_str());
    SetDlgItemTextW(g_wnd, kBounce, ReadIni(L"Env", L"CRYMT_GI_BOUNCE", L"1.0").c_str());
    SetDlgItemTextW(g_wnd, kAo, ReadIni(L"Env", L"CRYMT_GI_AO", L"0.8").c_str());
    SetDlgItemTextW(g_wnd, kTemporal, ReadIni(L"Env", L"CRYMT_GI_TEMPORAL", L"0.75").c_str());
    SetDlgItemTextW(g_wnd, kStatus, Status().c_str());
}

bool Save()
{
    const wchar_t* mode = IsDlgButtonChecked(g_wnd, kModeOff) ? L"off" : IsDlgButtonChecked(g_wnd, kModeDx12) ? L"dx12" : IsDlgButtonChecked(g_wnd, kModeDlaa) ? L"dlaa" : L"mt";
    WritePrivateProfileStringW(L"CryMT", L"Mode", mode, g_ini.c_str());
    WritePrivateProfileStringW(L"CryMT", L"Borderless", IsDlgButtonChecked(g_wnd, kBorderless) ? L"1" : L"0", g_ini.c_str());
    WritePrivateProfileStringW(L"CryMT", L"PauseOnFocusLoss", IsDlgButtonChecked(g_wnd, kPause) ? L"1" : L"0", g_ini.c_str());
    const bool gi = IsDlgButtonChecked(g_wnd, kGi) != 0;
    WritePrivateProfileStringW(L"CryMT", L"GI", gi ? L"1" : L"0", g_ini.c_str());
    WritePrivateProfileStringW(L"Env", L"CRYMT_GI_RADIUS", Text(kRadius).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"Env", L"CRYMT_GI_BOUNCE", Text(kBounce).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"Env", L"CRYMT_GI_AO", Text(kAo).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"Env", L"CRYMT_GI_TEMPORAL", Text(kTemporal).c_str(), g_ini.c_str());
    if (gi)
    {
        const std::wstring error = InstallGi();
        if (!error.empty())
        {
            MessageBoxW(g_wnd, (std::wstring(T(L"GI не включён: ", L"GI not turned on: ")) + error).c_str(), L"CryMT", MB_ICONWARNING);
            return false;
        }
    }
    else
        RemoveGi();
    return true;
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, int dpi)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(w, dpi, 96), MulDiv(h, dpi, 96),
                             g_wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}

void Build(int dpi)
{
    Make(L"BUTTON", T(L"Режим", L"Mode"), BS_GROUPBOX, 12, 8, 456, 118, -1, dpi);
    Make(L"BUTTON", T(L"Выключен (оригинальная игра)", L"Off (original game)"), BS_AUTORADIOBUTTON | WS_GROUP, 24, 28, 430, 20, kModeOff, dpi);
    Make(L"BUTTON", T(L"MT — рендер в отдельном потоке, D3D10", L"MT — render thread, D3D10"), BS_AUTORADIOBUTTON, 24, 50, 430, 20, kModeMt, dpi);
    Make(L"BUTTON", T(L"MT + D3D12 — свой бэкенд D3D12", L"MT + D3D12 — own D3D12 back end"), BS_AUTORADIOBUTTON, 24, 72, 430, 20, kModeDx12, dpi);
    Make(L"BUTTON", T(L"MT + D3D12 + NVIDIA DLAA (только RTX)", L"MT + D3D12 + NVIDIA DLAA (RTX only)"), BS_AUTORADIOBUTTON, 24, 94, 430, 20, kModeDlaa, dpi);
    Make(L"BUTTON", T(L"Полный экран как окно без рамки (мгновенный Alt+Tab)", L"Fullscreen as a borderless window (instant Alt+Tab)"),
         BS_AUTOCHECKBOX | WS_GROUP, 16, 134, 450, 20, kBorderless, dpi);
    Make(L"BUTTON", T(L"Меню паузы при уходе из игры", L"Pause menu when switching away"), BS_AUTOCHECKBOX, 16, 156, 450, 20, kPause, dpi);
    Make(L"BUTTON", T(L"GI (эксперимент, режимы D3D12)", L"GI (experimental, D3D12 modes)"), BS_GROUPBOX, 12, 182, 456, 108, -1, dpi);
    Make(L"BUTTON", T(L"Включить экранный GI (ставит патч шейдеров)", L"Screen-space GI (installs the shader patch)"), BS_AUTOCHECKBOX, 24, 202, 430, 20, kGi, dpi);
    const wchar_t* labels[4] = {T(L"Радиус, м", L"Radius, m"), T(L"Отражённый свет", L"Bounce"), T(L"Затенение", L"Occlusion"), T(L"Сглаживание по времени", L"Temporal")};
    for (int i = 0; i < 4; i++)
    {
        const int x = 24 + (i % 2) * 222, y = 228 + (i / 2) * 28;
        Make(L"STATIC", labels[i], 0, x, y + 3, 150, 20, -1, dpi);
        Make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, x + 152, y, 56, 22, kRadius + i, dpi);
    }
    Make(L"STATIC", L"", 0, 16, 298, 450, 64, kStatus, dpi);
    Make(L"BUTTON", T(L"Сохранить и запустить Crysis", L"Save and start Crysis"), BS_DEFPUSHBUTTON | WS_GROUP, 12, 368, 200, 28, kSaveStart, dpi);
    Make(L"BUTTON", T(L"Сохранить", L"Save"), 0, 218, 368, 90, 28, kSave, dpi);
    Make(L"BUTTON", T(L"Отчёты", L"Reports"), 0, 314, 368, 76, 28, kReports, dpi);
    Make(L"BUTTON", T(L"Закрыть", L"Close"), 0, 396, 368, 72, 28, kClose, dpi);
}

LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case kSave:
            if (Save())
                SetDlgItemTextW(w, kStatus, (std::wstring(T(L"Сохранено.\r\n", L"Saved.\r\n")) + Status()).c_str());
            return 0;
        case kSaveStart:
            if (Save())
            {
                const std::wstring bin = g_game + L"Bin64";
                // GI needs developer mode for now: without it the engine uses the shipped parsed shader scripts.
                ShellExecuteW(w, L"open", (bin + L"\\Crysis.exe").c_str(), L"-dx10", bin.c_str(),
                              SW_SHOWNORMAL);
                DestroyWindow(w);
            }
            return 0;
        case kReports:
            CreateDirectoryW((g_dir + L"reports").c_str(), nullptr);
            ShellExecuteW(w, L"open", (g_dir + L"reports").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kClose:
            DestroyWindow(w);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR args, int show)
{
    SetProcessDPIAware();
    g_ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1); // ...\CryMT\ .
    g_game = g_dir.substr(0, g_dir.find_last_of(L'\\', g_dir.size() - 2) + 1);
    g_ini = g_dir + L"CryMT.ini";
    // Without the window (scripted installs, tests): --gi-install / --gi-remove; exit code 0 on success.
    if (args && wcsstr(args, L"--gi-install"))
        return InstallGi().empty() ? 0 : 1;
    if (args && wcsstr(args, L"--gi-remove"))
    {
        RemoveGi();
        return 0;
    }
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"CryMTSettings";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, MulDiv(480, dpi, 96), MulDiv(408, dpi, 96)};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(L"CryMTSettings", L"CryMT v16", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    Build(dpi);
    Load();
    ShowWindow(g_wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
        if (!IsDialogMessageW(g_wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    return 0;
}
