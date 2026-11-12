// Crysis 1.2.1, build 6156, Bin64 DX10: addresses, object layouts and vtable slots found by reversing the
// shipped DLLs (CryRenderD3D10.dll at 0x38000000, Cry3DEngine.dll at 0x30000000). All of it is specific to
// this build; Warhead and Wars need their own copy (find the same functions by byte signature).
// Offsets are in bytes. "TArray" and "PodArray" are {T* data; int count; int allocated}.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ce2
{
// ---------------------------------------------------------------------------------------------------------
// CryRenderD3D10.dll
// ---------------------------------------------------------------------------------------------------------
constexpr uintptr_t kRenderBase = 0x38000000;
constexpr uintptr_t kRenderSize = 0xA9E000;
constexpr uintptr_t kRendererVtable = 0x38256DB8;   // CD3D9Renderer, 222 slots
constexpr uintptr_t kRenderObjectVtable = 0x38253840; // CRenderObject
constexpr uintptr_t kRenderMeshVtable = 0x38269BC8;   // CRenderMesh, 72 slots
constexpr uintptr_t kAuxVtable = 0x38252B28;          // CD3DRenderAuxGeom, 29 slots; overloads in reverse declaration order
constexpr uintptr_t kAuxFirstFn = 0x380887E0;
constexpr int kAuxSlots = 29;

// Functions.
constexpr uintptr_t kEndEf3D = 0x380ADC40;          // EF_EndEf3D, the flush
constexpr uintptr_t kPreprocessFn = 0x380A6D20;     // flush preprocess: the only stage that calls back into Cry3DEngine
constexpr uintptr_t kPreprocessCall = 0x380AA61F;   // ... called from here
constexpr uintptr_t kShadowPrepFn = 0x380BEE30;     // (renderer): draws shadow casters, terrain sectors through Cry3DEngine
constexpr uintptr_t kShadowPrepCall = 0x380AA636;   // ... called right after preprocess unless kShadowPrepGlobal is 0 or flags & 8
// CD3D9Renderer::vf207 PrepareDepthMap(frustum, light), per shadow frustum: per side, viewport and matrices, EF_StartEf,
// casters' Render (frustum +0x110 list) on this thread, renderer +0x1788 = frustum, nested EF_EndEf3D draws the map.
constexpr uintptr_t kPrepareDepthMapFn = 0x380BC560;
constexpr uintptr_t kPrepareDepthMapSize = 10433;
// FlushShadowPrep: per light of the level with flags & 0x10, PrepareDepthMap per frustum (light +0x1F0), unless
// g 0x382E9A04 is set and flags & 2 (then FUN_380BA870). PrepareDepthMap's caster loop (rdi = frustum, rbx = caster,
// IShadowCaster: slot 0 test, slot 1 Render(SRendParams&), slot 2 GetBBox(AABB*), flags at +0xC) builds SRendParams at
// rsp+0x2C0: nTechniqueID 4, fAlpha 1, nRenderList 2, fDistance camera to box, dwFObjFlags 0xF (| 0x200 with caster
// flag bit 9), pRenderNode caster, fRenderQuality 1, nAfterWater 1; the camera is ISystem::GetViewCamera (slot
// 0x280/8 of g 0x382E6A98, matrix translation at +0xC/+0x1C/+0x2C). The call: mov rax,[rbx]; lea rdx,[rsp+0x2C0];
// mov rcx,rbx; call [rax+8] at 0x380BE98A (17 bytes).
constexpr uintptr_t kShadowStaticGlobal = 0x382E9A04;
// The renderer's environment cubes ($EnvCMap_%d): a pool of 16 SEnvTexture, 200 bytes each (0x38640620..0x386412A0;
// FUN_3813F840 makes their textures at the start, the 8x8 pool at 0x3863F9A0 and the 2D one at 0x386412A0 alike). +0 char busy (the pool's
// reuse skips busy ones, its render returns at once), +1 char all faces drawn once, +8 int the next face, +0x18 SDynTexture*
// (its CTexture at +0x98: the D3D texture at +0x28, its view at +0x38), +0x20 float the time of the last update, +0x24 the position for the pool's nearest-cube
// lookups (materials with an automatic cube get the nearest), +0x30 the position the render uses when the renderer has no
// render element (kRdRendElement null; else the element's). FUN_38132490 is the pool (nearest, update interval, budget).
constexpr uintptr_t kEnvCubePool = 0x38640620;
constexpr size_t kEnvCubeEntryBytes = 0xC8;
constexpr int kEnvCubeEntries = 16;
// FUN_380D4F00(entry, draw flags, -1, small): the cube's faces, all 6 while entry +1 is 0, else the face at +8, each into
// TempEnvCMap through I3DEngine::RenderWorld(3, camera, "DrawCubeSide", flags & ~DLD_ENTITIES) (FUN_380CF8D0: 90 degrees,
// the 3D engine's view distance), then Encode_ToLDR into the cube's face (RGBK: rgb / max, a = max / 16). The size from
// r_EnvCMResolution (0 64, 1 128, 2 256, 3 512). Returns 1.
constexpr uintptr_t kEnvCubeRenderFn = 0x380D4F00;
constexpr uintptr_t kRdCameraX = 0x28D14;            // the camera's position: floats at +0x28D14, +0x28D24, +0x28D34
// The renderer's temporary depth surfaces (FUN_380212B0 FX_GetDepthSurface(w, h, aa): the first free one of that size,
// else a new D3D texture and view): pointers at +0x2D848, count +0x2D850; each {int w, h; char busy at +8; ...}. The
// busy flag is set by FX_SetRenderTarget (FUN_3801CF80) and cleared when a target slot gets another depth or is popped.
constexpr uintptr_t kRdDepthPool = 0x2D848;
constexpr uintptr_t kRdDepthPoolCount = 0x2D850;
constexpr uintptr_t kTextureDevice = 0x28;           // CTexture: the D3D texture (ID3D10Resource*); +0x38 its view
// CTexture +0x30: its render target view, made by FUN_380CC480 (for a cube: the face asked first, as a 1-slice array)
// and then given for every face: the engine's real-time cube (a feature cut from the shaders) drew all faces into one.
constexpr uintptr_t kTextureRtv = 0x30;
constexpr uintptr_t kRenderSystemGlobal = 0x382E6A98;
constexpr uintptr_t kCasterRenderCall = 0x380BE98A;
constexpr uint8_t kCasterRenderCode[] = {0x48, 0x8B, 0x03, 0x48, 0x8D, 0x94, 0x24, 0xC0, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x08};
constexpr uintptr_t kFrustumCasters = 0x110; // PodArray<IShadowCaster*>*
constexpr uintptr_t kBatchListFn = 0x380A4480;      // FX_ProcessBatchesList
constexpr uintptr_t kTextureDataSizeFn = 0x38131D50; // int (w, h, depth, mips, format): bytes of the texture data, pure
// SetCamera (0x380E25B0) builds the view matrix on the model-view stack and a right-handed D3DX-style projection on
// the projection stack (m23 = -1; a tiled-screenshot branch offsets m20/m21), then calls FUN_3809DDF0 with the renderer:
// it copies view to +0x10/+0x50 and projection to +0x110 and derives view-projection (+0x190) and its inverse (+0x210).
// At that call the CCamera* is in rdi and SetCamera's frame is 0x208 bytes.
constexpr uintptr_t kSetCameraMatricesCall = 0x380E3215;
constexpr uintptr_t kViewProjUpdateFn = 0x3809DDF0;
constexpr uintptr_t kCamFov = 0x30;      // CCamera: float, radians (vertical)
constexpr uintptr_t kCamAspect = 0x3C;   // float, projection ratio
constexpr uintptr_t kCamNear = 0x48;     // float
constexpr uintptr_t kCamFar = 0x60;      // float
constexpr uintptr_t kCamWidth = 0x1E8;   // int, viewport width
constexpr uintptr_t kCamHeight = 0x1EC;  // int, viewport height
constexpr int kListHdrPostProcess = 8;   // EFSLIST_HDRPOSTPROCESS (Mod SDK IShader.h): tone mapping, bloom
constexpr uintptr_t kMatricesDirtyJump = 0x3809DF77; // tail jump of FUN_3809DDF0 (view-projection update) ...
constexpr uintptr_t kMatricesDirtyFn = 0x3803A390;   // ... to this: sets bits 0x202000 at +0xAF8 of the global renderer

// FX pipeline (the flush's batch processing).
// Render items: arrays at (&kRenderItemLists)[(afterWater * 15 + list) * 2], kRenderItemSize bytes each: +0 sort key
// (bits 6..17 shader index into *kShaderTable, entry - 8 = CShader*; bits 0..5 technique, 0x3F none; bits 18.. shader
// resources index into kShaderResTable), +8 CRendElement*, +0x18 CRenderObject*, +0x24 batch flags (filter bits).
constexpr uintptr_t kRenderItemLists = 0x382EA040;
constexpr size_t kRenderItemSize = 0x28;
constexpr uintptr_t kShaderTable = 0x38659290;
constexpr uintptr_t kShaderResTable = 0x38659318;
constexpr uintptr_t kZPrepassFn = 0x380A5470;        // (renderer): lists 2, 4, 6 before/after water with filter 8
constexpr uint8_t kZPrepassHead[] = {0x40, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x28};
constexpr uintptr_t kBatchStartFn = 0x380A1360;      // (renderer, shader, technique, resources, RE)
constexpr uintptr_t kObjectChangeFn = 0x380A4080;    // (renderer, shader, resources, object, RE) -> bool
constexpr uint8_t kObjectChangeHead[] = {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};
constexpr uint8_t kFlushShaderHead[] = {0x40, 0x55, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00};
constexpr uint8_t kDrawObjectsHead[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10};
constexpr uintptr_t kDrawObjectsAltFn = 0x38025690;  // the other per-object path of DrawShader_Generic
constexpr uint8_t kDrawObjectsAltHead[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56};
constexpr uintptr_t kInstanceMergeFn = 0x380A11F0;   // (renderer, object, previous object, RE) -> bool
constexpr uintptr_t kFlushShaderFn = 0x3802D6D0;     // the batch flush (renderer +0xB28 points here in the z-prepass)
constexpr uintptr_t kDrawGenericFn = 0x38028BE0;     // "DrawShader_Generic": per pass FUN_3801BA60, then the object loop
constexpr uintptr_t kDrawObjectsFn = 0x380268D0;     // "DrawShader_BatchStatic/BatchSkinned": per-object loop
constexpr uintptr_t kPassFlagsFn = 0x3801BA60;       // runtime flag mask for the pass, then renderer vf 0x6B0
// Shader parameters: (SCGParam* params, int count, void*, int stage, int cbSlot); SCGParam is 0x30 bytes: +8 flags,
// +0xC register (int16), +0x10 vectors, +0x18 type (one byte per component), +0x20 data. Scratch in globals.
constexpr uintptr_t kSetParamsFn = 0x3803E670;
constexpr uint8_t kSetParamsHead[] = {0x44, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr size_t kParamSize = 0x30;
constexpr uintptr_t kParamNameFn = 0x3816C6C0;       // (unused, type) -> const char*, the parameter type's name
// CHWShader_D3D vf12 (+0x60): (shader, group) sets one parameter group of the current instance (shader +0x70):
// params = *(instance + 0x38 + group * 8) -> {begin, end} of SCGParam, cb vectors = *(instance + 0x70 + group * 4),
// stage = *(shader + 0x20). Group 1 = per instance (the per-object loops call it for VS, PS and GS).
constexpr uintptr_t kShaderSetGroupFn = 0x380443B0;
constexpr uint8_t kShaderSetGroupHead[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x41, 0x70};
// (reg floats, slot, stage, data, floats, cb vectors): copies into the mapped constant buffer of that size.
constexpr uintptr_t kCbWriteFn = 0x3801C0F0;
constexpr uint8_t kCbWriteHead[] = {0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x56, 0x57};
// CREMesh vf15 mfDraw (re, shader-ish, pass): with p2 +0x60 != 0 -> FUN_38024D80(renderer, primitive type: mesh +0x100,
// or 6 when renderer +0x12924 >= 0); else renderer vf34 (DrawIndexMesh with the mesh's buffers).
constexpr uintptr_t kMeshDrawFn = 0x38091A20;
constexpr uintptr_t kCREMeshVtable = 0x38258F58; // 22 slots; in fill mode CryMT's thunks replace its entries
constexpr uint8_t kMeshDrawHead[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50};
// FUN_38024D80 (renderer, primitive type) "Draw_DrawCall": FUN_380452F0(1) commits the constant buffers (27 stage/slot
// pairs, then the material and light buffers), FUN_38024810(renderer, 0) the render targets; no draw while the
// cvar at kDrawDisabled is set or the current VS/PS (or GS) instance is missing or not ready (+0x8A); type 0: triangle
// list (topology 4, cached at renderer +0x2C290), DrawIndexed(+0x12928, +0x12934 + +0x12930, 0); statistics at
// renderer +0x13FD0 (triangles) and +0x13F94 (draws) indexed by the list at +0xF08.
constexpr uintptr_t kCommitConstantsFn = 0x380452F0;
constexpr uintptr_t kSetTargetsFn = 0x38024810;
constexpr uintptr_t kDrawDisabled = 0x382E97B8;
constexpr uintptr_t kCurVsInstance = 0x382D80B0;
constexpr uintptr_t kCurPsInstance = 0x382D78A8;
constexpr uintptr_t kCurGsInstance = 0x382D81C8;
constexpr uintptr_t kInstanceNotReady = 0x8A;
constexpr uintptr_t kRdChunkStream = 0x12924; // < 0: draw the mesh's own index range
constexpr uintptr_t kRdIndexCount = 0x12928;
constexpr uintptr_t kRdFirstIndex = 0x12930;
constexpr uintptr_t kRdIndexBase = 0x12934;
constexpr uintptr_t kRdTopology = 0x2C290;
constexpr uintptr_t kRdDevice = 0x2C270;
constexpr uintptr_t kRdStatList = 0xF08;
constexpr uintptr_t kRdStatTriangles = 0x13FD0;
constexpr uintptr_t kRdStatDraws = 0x13F94;
constexpr uintptr_t kMeshPrimitive = 0x100;  // CRenderMesh: primitive type (0 triangle list)
constexpr uintptr_t kMeshVertexFormat = 0xFC; // CRenderMesh: vertex format
// The z-prepass's six FX_ProcessBatchesList calls (lists 2, 4, 6 before and after water).
constexpr uintptr_t kZPrepassListCalls[6] = {0x380A5677, 0x380A56A3, 0x380A56F1, 0x380A571D, 0x380A576B, 0x380A5797};
constexpr uintptr_t kObjDataArrayPtr = 0x382AFDB0; // pointer to the frame's 0x20-byte objdata entries (object +0xC index)
constexpr uintptr_t kDeclSetupFn = 0x3801CCA0;     // (renderer) -> flag for the restore FUN_3809DF80 after the objects
constexpr uintptr_t kDeclSetFn = 0x3801CB80;       // (renderer, +0xB00, +0x180C) -> < 0: nothing to draw
constexpr uintptr_t kDeclRestoreFn = 0x3809DF80;   // (renderer, {renderer +0x28808 qword, +0x28810, +0x28814})
// SShaderTechnique: +8 passes (0x20 bytes each: +8 VS, +0x10 GS, +0x18 PS CHWShader*), +0x10 pass count.
// CHWShader: +0x20 stage (0 VS, 1 PS, 2 GS), +0x70 current instance. CShader +0x38: draw type (0 generic).
constexpr uintptr_t kRdObjMatrix = 0xA80;    // const float* (3x4) of the object being drawn
constexpr uintptr_t kRdObjFlagsA = 0xAF4;    // PI_ObjMatrix is camera-relative unless bit 20 here ...
constexpr uintptr_t kRdObjFlagsB = 0xAEC;    // ... and when bits 0..2 are set and bit 10 is clear here
constexpr uintptr_t kRdRelativeOrigin = 0x28F44; // float x, y, z subtracted from camera-relative matrices
constexpr uintptr_t kRendererGlobal = 0x382E7530;    // CD3D9Renderer* (gRenDev)
constexpr uintptr_t kRdShader = 0xA68;               // CShader* of the batch (vtable +0x28: GetName)
constexpr uintptr_t kRdShaderRes = 0xAB0;           // SRenderShaderResources* of the batch
constexpr uintptr_t kRdCurObject = 0xA78;            // CRenderObject* being drawn
constexpr uintptr_t kRdRendElement = 0xA88;
constexpr uintptr_t kRdTechnique = 0xAA8;
constexpr uintptr_t kRdBatchObjects = 0xAC0;         // TArray<CRenderObject*> of the batch
constexpr uintptr_t kRdBatchFilter = 0xF00;
constexpr uintptr_t kRdFlushCallback = 0xB28;

// Globals.
constexpr uintptr_t kRecurseLevel = 0x382EA030;     // int, current EF_StartEf recursion level (0 = top)
constexpr uintptr_t kShadowPrepGlobal = 0x382E9A18; // shadow prep (FUN_380BEE30) is skipped while this is zero
constexpr uintptr_t kObjEntryArray = 0x382E9B88;    // shader parameter entries (0x14 bytes) of the frame's objects
constexpr uintptr_t kEmptyArray = 0x382DD0F8;       // empty TArray header sentinel (SRenderObjData +0x18)
constexpr uintptr_t kCvarUseGSParticles = 0x382E9730; // int, r_UseGSParticles
constexpr uintptr_t kConstOne = 0x3824CCA0;         // float 1.0 the renderer stores into fresh objects

// CD3D9Renderer object: allocated with 0x2D8D4 bytes (0x380E7CB8), placed 16-aligned at +0x14.
constexpr size_t kRendererObjectSize = 0x2D8C0;
constexpr uintptr_t kRdBatchListId = 0xF04;          // list id stored by FX_ProcessRenderList (FUN_380A5390)
constexpr uintptr_t kRdLevelLights = 0xF40;          // TArray<CDLight*> per recursion level: + level * 16
constexpr uintptr_t kRdPolyVerts = 0x12900;          // TArray<uint8_t> of client polygon vertices (EF_AddPolygonToScene)
constexpr uintptr_t kRdPolyIndices = 0x12910;        // TArray<uint16_t>
constexpr uintptr_t kRdVisibleObjectCount = 0x141A0; // int, objects in the frame list (max 0x4000)
constexpr uintptr_t kRdVisibleObjects = 0x141A8;     // CRenderObject* array, the frame list (object +0xA = index)
constexpr uintptr_t kRdFreedObjects = 0x141B8;       // TArray<CRenderObject*> of temp objects merged away
constexpr uintptr_t kRdPermanentObjects = 0x141C8;   // TArray<CRenderObject*>, EF_GetObject(false, id)
constexpr uintptr_t kRdTempObjects = 0x141D8;        // TArray<CRenderObject*>, EF_GetObject(true, -1)
constexpr uintptr_t kRdMergedParticleObjects = 0x14210; // TArray, FUN_38103600
constexpr uintptr_t kRdMergedPolyObjects = 0x14220;  // TArray, FUN_38102590
constexpr uintptr_t kRdFog = 0x287F0;                // fog fields +0x287F0..+0x28814 (SetFog)
constexpr uintptr_t kFogFlagOffset = 0x287F8;
constexpr uintptr_t kRdColorBits = 0x28CEC;          // int, set by Init and resolution changes
constexpr uintptr_t kCameraOffset = 0x28D08;         // CCamera (SetCamera copies 0x218 bytes)
constexpr size_t kCameraArgBytes = 0x218;
constexpr size_t kCameraBytes = 0x400;
constexpr uintptr_t kFrameIdAllOffset = 0x28F68;
constexpr uintptr_t kRdFogContributions = 0x28FA8;   // std::vector<ColorF> (begin, end, capacity), cleared by top EF_StartEf
constexpr uintptr_t kRdGpuCount = 0x28FCC;           // int
constexpr uintptr_t kFrameIdOffset = 0x291E8;
constexpr uintptr_t kRdShaderQuality = 0x29558;      // int per shader type, stride 8
constexpr uintptr_t kRdDeferredTextures = 0x29918;   // two COM pointers (+0x29918, +0x29920), slot 198
constexpr uintptr_t kSwapChainOffset = 0x299C8;      // IDXGISwapChain*, Present at 0x380E68EE
constexpr uintptr_t kDeviceOffset = 0x2C270;         // ID3D10Device*
constexpr uintptr_t kViewportOffset = 0x2C2AC;
constexpr uintptr_t kModelViewStackOffset = 0x2CD50; // matrix stack pointers (top, base, depth, capacity, flags)
constexpr uintptr_t kProjStackOffset = 0x2CD58;
constexpr uintptr_t kRdTerrainAONodes = 0x2D870;     // PodArray<SSectorTextureSet>
constexpr uintptr_t kAuxObjectOffset = 0x2D898;      // CD3DRenderAuxGeom*
constexpr uintptr_t kAuxFlagsOffset = 0x88;          // in the aux geometry object

// CRenderObject: 0x130 bytes, allocated 0x144 and placed 16-aligned at +0x14. Temp objects live one frame;
// permanent ones keep their id (flag 0x2000 marks a freed one, reused in place).
constexpr size_t kRenderObjectBytes = 0x130;
constexpr uintptr_t kObjId = 0x8;           // uint16, index in the temp or permanent pool
constexpr uintptr_t kObjVisibleIndex = 0xA; // int16, index in the frame list
constexpr uintptr_t kObjFlags = 0x10;       // uint32
constexpr uintptr_t kObjMatrix = 0x14;      // Matrix34
constexpr uintptr_t kObjColor = 0x44;       // 4 floats
constexpr uintptr_t kObjLod = 0x88;         // uint8
constexpr uintptr_t kObjAlpha = 0xF8;       // float
constexpr uintptr_t kObjEntryIndex = 0xD2;  // uint16, first of two entries in kObjEntryArray (0 = none)
constexpr uintptr_t kObjCharInstance = 0x110; // ISkinnable*, not referenced
constexpr uintptr_t kObjLight = 0x128;      // CDLight* of a light-shader object (EF_ADDDlight)
constexpr uint32_t kObjFlagFree = 0x2000;
constexpr size_t kObjEntryBytes = 0x28;     // the object's two shader parameter entries
constexpr size_t kObjDataBytes = 0x20;      // SRenderObjData (CRenderObject slot 1)
constexpr uint32_t kPermObjectMax = 0x4000;

// CDLight: 0x258 bytes. Shadow frustums (0x388 bytes each) in an array of up to 8 at +0x1F0.
constexpr size_t kLightBytes = 0x258;
constexpr size_t kFrustumBytes = 0x388;
constexpr uintptr_t kLightId = 0x0;           // int, set by EF_ADDDlight (-1 for fake or past 32 lights)
constexpr uintptr_t kLightOrigin = 0x4;       // Vec3
constexpr uintptr_t kLightColour = 0x1C;      // ColorF (HDR, the sun's 8.25 6.90 5.16 1 on island at 7:00); again at +0x2C
constexpr uintptr_t kLightMatrix = 0xE4;      // Matrix34 copied into the light-shader object
constexpr uintptr_t kLightObjects = 0x118;    // CRenderObject* per recursion level (light shaders)
constexpr uintptr_t kLightShaderItem = 0x1B0; // SShaderItem
constexpr uintptr_t kLightFlags = 0x1D4;      // EF_IsFakeDLight reads these
constexpr uintptr_t kLightFrustums = 0x1F0;
constexpr uintptr_t kLightRadius = 0x50;        // float, metres (the plane's lamps 12, 3.5, 2, 9.5; the sun 1e8); again at +0x54
constexpr uintptr_t kLightProjectorFov = 0x1A0; // float, a projector's full angle in degrees (59.75 in the plane; 45 default)
// Flags (kLightFlags) seen in the island intro: 0x02 the sun, 0x10 shadow maps (FlushShadowPrep takes these), 0x20 a
// point light, 0x40 a projector (its direction the first column of kLightMatrix); bits 23 and 17 make it fake.
constexpr uint32_t kLightFlagSun = 0x2, kLightFlagPoint = 0x20, kLightFlagProjector = 0x40;
constexpr uint32_t kLightFlagsFake = (1u << 23) | (1u << 17);
// Every light entity of the level: C3DEngine (k3DEngineGlobal) + 0x788 a PodArray of CLightEntity* (count + 0x790; the
// destructor 0x301AD120 takes it out through 0x300FEF80, which also clears the sun's entity at + 0x800). A light
// entity's CDLight is at + 0x38 (its constructor 0x301AE8D0 builds it there). Village: 19 entities, 4 of them in a
// frame's list (lamps of the next rooms are not). + 0x8 is not the render flags (float-like values).
constexpr uintptr_t kEngineLightEntities = 0x788, kEngineSunEntity = 0x800, kLightEntityLight = 0x38, kRenderNodeFlags = 0x8;
// VisAreas: CVisAreaManager* at 0x3022B910 (made by 0x3011ED90, vtable 0x301D5738): VisAreas + 0x28 (count + 0x30),
// portals + 0x38 (count + 0x40), occlusion areas + 0x48 (+ 0x50) (CRYMT_VISDUMP on village: 23 "visareaN", 54
// "portal_portalN"). CVisArea (Update = vf14 0x30126B20): box min + 0x20,
// max + 0x2C, name + 0x58 (32 chars), shape points + 0xF0 (Vec3 array, count + 0xF8), height + 0x100, then the
// SVisAreaInfo fields from + 0x104 (Vec3, the ambient colour by its place), + 0x110 byte, + 0x118 / 0x119 bytes,
// + 0x11C float, + 0x120 / 0x121 / 0x17C bytes; + 0x17D portal (name has "portal"), + 0x17E ignore sky.
constexpr uintptr_t kVisAreaManagerGlobal = 0x3022B910;
constexpr uintptr_t kVamVisAreas = 0x28, kVamPortals = 0x38;
constexpr uintptr_t kVisAreaBox = 0x20, kVisAreaName = 0x58, kVisAreaPoints = 0xF0, kVisAreaHeight = 0x100, kVisAreaAmbient = 0x104;

// SShaderItem: {CShader*, SRenderShaderResources*, int technique}, 0x18 bytes. CShader techniques: pointer
// array at +0x58, count at +0x60 (FUN_3816ED90; a negative index means 0). Technique flags at +0x18.
constexpr size_t kShaderItemBytes = 0x18;
constexpr uintptr_t kShaderTechniques = 0x58;
constexpr uintptr_t kShaderTechniqueCount = 0x60;
constexpr uintptr_t kTechniqueFlags = 0x18;
constexpr uint32_t kTechniqueGSParticles = 0x100000; // EF_AddParticlesToScene keeps canUseGS only with it

// CRenderMesh fields.
constexpr uintptr_t kMeshMaterial = 0x30;   // IMaterial*, AddRef'd
constexpr uintptr_t kMeshChunks = 0x108;    // TArray<CRenderChunk>*: 0x48 bytes each, render element at +0x20
constexpr uintptr_t kMeshIndices = 0xE8;    // uint16* system copy
constexpr uintptr_t kMeshIndexCount = 0xF0; // uint32
constexpr uintptr_t kMeshBBoxMin = 0x130;   // Vec3
constexpr uintptr_t kMeshBBoxMax = 0x13C;   // Vec3
constexpr size_t kMeshChunkBytes = 0x48;
// CRenderMesh slots (vtable order).
constexpr int kRenderMeshSlots = 72;
enum MeshSlot : int
{
    kMeshRelease = 1,        // 0x3815FA10, destructor at 0 references
    kMeshStreamPtr = 9,      // 0x381580D0 (stream, stride*, ?, lock): lock false only reads the system copy
    kMeshStreamPtr2 = 11,    // 0x381581E0, same
    kMeshGet110 = 19,        // 0x3815A100 returns +0x110
    kMeshSetChunkRe = 25,    // 0x38157E80 sets every chunk's render element +0x10
    // 0x3815D070 (material, firstIdx, numIdx, firstVert, numVert, chunk id, flag): builds a chunk and passes it to
    // slot 27, which may reallocate the chunk array and create render elements. All arguments by value.
    kMeshSetChunk = 26,
    kMeshSetIndices = 30,    // 0x3815A950 (inds, count): reallocates the system copy, marks the stream (slot 39)
    kMeshSetMaterial = 34,   // 0x38157DF0 (material, chunk value): material +0x30, chunk values when nonzero
    kMeshSetBBox = 36,       // 0x3815A130 (min, max)
    // 0x3815B310 (container): moves the mesh from its vertex container's dependent list (+0x88, count +0x90) to the
    // new container's; terrain surface meshes (CTerrainNode::UpdateSurfaceRenderMeshes, 3D+0xC0A50) use the sector mesh.
    kMeshSetVertexContainer = 42,
    kMeshSetB0 = 43,         // 0x3815A1C0 sets the int at +0xB0 (the surface update passes the index count)
    kMeshRender = 47,
    kMeshTriangleAreas = 59, // 0x3815FA40 triangle area sums from the system copies into the caller's struct
    kMeshRandomPoint = 60,   // 0x38160580 a random point on the mesh (renderer RNG)
};
// CRenderMesh constructor (this, type name, source name): links the mesh at the tail of the global mesh list
// (sentinel 0x38657B10, tail 0x38657B48; +0x38 prev, +0x40 next). The destructor (called by Release, slot 1) unlinks
// it from its neighbours and releases the chunks' render elements. Both start with whole position-independent
// instructions of 5 and 9 bytes.
constexpr uintptr_t kMeshCtorFn = 0x38159E50;
constexpr uint8_t kMeshCtorHead[] = {0x48, 0x89, 0x5C, 0x24, 0x08};                         // mov [rsp+8], rbx
constexpr uintptr_t kMeshDtorFn = 0x3815B460;
constexpr uint8_t kMeshDtorHead[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x89, 0x5C, 0x24, 0x40}; // sub rsp,0x38; mov [rsp+0x40],rbx
constexpr uintptr_t kREMeshVtable = 0x38258F58; // CREMesh: +0x40 chunk, +0x48 mesh
constexpr int kRETypeMesh = 9;                  // EF_CreateRE(9) = CREMesh, 0x50 bytes (slot 111)
constexpr size_t kSectorTextureSetBytes = 0x48; // SSectorTextureSet (SetTerrainAONodes)
constexpr size_t kDynVertexSize = 0x18;         // struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F
constexpr size_t kTangentsBytes = 0x10;         // SPipTangents

// CD3D9Renderer vtable slots. The SDK's IRenderer.h order holds up to slot 92 and is one ahead after it
// (SDK 88 = slot 87); the names below were checked by decompiling the targets.
enum Slot : int
{
    kBeginFrame = 17,
    kRenderDebug = 18,
    kEndFrame = 19,
    kGetViewport = 20,
    kSetViewport = 21,
    kGetDynVBPtr = 28,
    kDrawDynVB = 30,
    kSetCamera = 43, // 0x380E25B0: copies the CCamera to +0x28D08, loads view and projection
    kGetCamera = 44,
    kSetTexture = 50,
    kDraw2dImage = 54,
    kProjectToScreen = 65,
    kUnProjectFromScreen = 67, // 0x380DE200: calls GetModelViewMatrix, GetProjectionMatrix, GetViewport on this
    kGetModelView = 68,
    kGetProjection = 69,
    kFontSetTexture = 78,
    kFontSetState = 80,
    kFontSetBlending = 81,
    kFontRestoreState = 82,
    // 0x38104840 EF_AddParticlesToScene(SShaderItem&, object, IParticleVertexCreator*, SParticleRenderInfo&, nAW,
    // bool& canUseGS): merges the object (FUN_38103600), takes a CREParticle with the creator and 24 bytes of the
    // render info (FUN_38128430); canUseGS = in && r_UseGSParticles && technique flag kTechniqueGSParticles.
    kAddParticles = 87,
    // 0x38102790 EF_AddPolygonToScene(SShaderItem&, numPts, verts, tangs, object, inds, ninds, nAW, bMerge): copies
    // into kRdPolyVerts/kRdPolyIndices, a CREClientPoly from the pool of the recursion level; returns the object or,
    // with bMerge, an equal one merged earlier (FUN_38102590).
    kAddPolygon = 92,
    kCreateRE = 111,      // 0x38108230 EF_CreateRE(type): new render element (CRendElement constructors link it into a global list)
    kStartEf = 112,       // 0x38109920; at level 0 resets the per-frame pools and the fog contributions
    kEfGetObject = 113,   // 0x38102010 (temp, id): see kRdTempObjects/kRdPermanentObjects; appends to the frame list
    kEfAddEf = 114,       // 0x38101FF0 -> FUN_38101D80(renderer, pRE, SShaderItem*, object, list, after water)
    kIsFakeDLight = 116,  // 0x380FD720, reads kLightFlags
    kAddDLight = 117,     // 0x38103E10: id = level light count (-1 when fake or past 32), stores the CDLight* in kRdLevelLights
    kUpdateDLight = 118,  // 0x381008E0, updates the light's projector animation
    kAddFillLight = 119,  // 0x381041E0 (const SFillLight&, 24 bytes): appended to the level's list (renderer+0x17A0+level*16)
                          // when its cvar (0x382E99C8) is on and the list holds < 512; only the flush reads the list
    kEfQuery = 121,       // 0x3810A730 (id, param)
    kSetClearColor = 124,
    kCreateRenderMesh = 126, // 0x381042E0, 17 arguments
    kDeleteRenderMesh = 127,
    kGetFrameID = 128,
    kDraw2dLabel = 133,
    kSetStateWrap = 136,
    kSetCullMode = 137,
    kSwapFogFlag = 138,
    kSetFog = 139,           // 0x380DDCC0 (density, start, end in xmm1..3, const float* color, mode): kRdFog
    kSetColorOp = 141,
    kSetTerrainAONodes = 142, // 0x380E5A60: copies a PodArray<SSectorTextureSet> to kRdTerrainAONodes
    // 0x380E2190 DownLoadToVideoMemory(data, w, h, src format, dst format, mips, repeat, filter, id, name, flags): with
    // id > 0 updates level 0 of that texture (UpdateTextureRegion 0x380D44F0) and returns the id, else creates one.
    kDownloadToVideoMemory = 149,
    kGetIRenderAuxGeom = 143,
    kEnableTMU = 147,
    kSelectTMU = 148,
    kResetToDefault = 156,
    kSetMaterialColor = 157,
    kFlushTextMessages = 160,
    kClearBuffer = 165,      // 0x380DE420 (flags, const ColorF* or null, depth in xmm3); GRendererXRender::BeginSubmitMask
                             // clears the stencil (4, null) for every Flash mask
    kSet2DMode = 171,
    // Scaleform (GFx) renderer calls of GRendererXRender (CrySystem), the HUD's Flash movies:
    kSfConfigMask = 183,     // 0x380FDCB0 (mask op, stencil ref): stencil state only
    kSfDrawTriList = 184,    // 0x380ACB10 (base vertex, min vertex, vertex count, start index, triangles, params)
    kSfDrawLineStrip = 185,  // 0x380AD140 (base vertex, lines, params)
    kSfDrawGlyphClear = 186, // 0x380AD620 (params)
    kSfFlush = 187,          // 0x380ADB80, no arguments
    // 0x38109690 PushFogVolumeContribution(const ColorF&): appends to kRdFogContributions, returns the index or
    // 0xFFFF when full.
    kPushFogContrib = 194,
    // Stores two COM pointers at kRdDeferredTextures (AddRef new, Release old); RenderWorld calls it first, with
    // nulls when a cvar is 0 (0x30110FB0). Named after what it does, the IRenderer name is unknown.
    kSetDeferredTextures = 198,
    kResetLevelField = 213,  // 0x380FF2C0, zeroes the level's light count (kRdLevelLights + 8 + level * 16)
    kSetState = 214,
};
constexpr int kEndEf3DSlot = 0x398 / 8;

// EF_Query ids.
constexpr uint32_t kQueryRecurseLevel = 0x13;
constexpr uint32_t kQueryLevelLight = 0x1B; // n-th light of the current level (kRdLevelLights), 0 past the end
// 0x17: stores renderer slot 201's bool into kQueryFlag17 and returns its address. CrySystem (+0xD95C7) asks twice a
// frame in the game update; slot 201 is a shared "return false" stub (0x38091100) in this build, so the answer is fixed.
constexpr uint32_t kQueryConstFlag = 0x17;
constexpr int kRendererSlot201 = 201;
constexpr uintptr_t kReturnFalseStub = 0x38091100;
constexpr uintptr_t kQueryFlag17 = 0x382E9BEC;
constexpr size_t kFillLightBytes = 24; // SFillLight as EF_ADDFillLight copies it (6 floats)
// SSF_GlobalDrawParams as the Scaleform draws (and the shader parameter setter FUN_3803E670 through renderer+0x28FC0)
// read it: +0 fill type, +4 vertex format (1/2/3/4: 4/8/12/20-byte vertices), +8 vertices, +0x10 vertex count,
// +0x18 16-bit indices, +0x20 index count, +0x28 transform matrix pointer (64 bytes, inside GRendererXRender),
// +0x30/+0x68 texture ids each with a texgen matrix after it, +0xA0 colour transform, +0xC0..+0xCC flags.
constexpr size_t kSfParamsBytes = 0xD0;
constexpr size_t kSfMatrixBytes = 64;

// CD3DRenderAuxGeom slots.
constexpr int kAuxSetFlags = 0;
constexpr int kAuxGetFlags = 1;
constexpr int kAuxNoOp = 24; // DrawBone(Matrix34), empty
constexpr int kAuxFlush = 26;

// ---------------------------------------------------------------------------------------------------------
// Cry3DEngine.dll
// ---------------------------------------------------------------------------------------------------------
constexpr uintptr_t kRenderWorldFn = 0x30110E10;   // C3DEngine::RenderWorld
constexpr uintptr_t kRenderSceneFn = 0x3010E590;   // C3DEngine::RenderScene (vtable slot 0x5B8/8)
constexpr uintptr_t kDisplayApiName = 0x301D55B4;  // "DX10" of the r_DisplayInfo line ("%s %dbit %s Build=%d..."), one reference (0x301119D5)
constexpr uintptr_t kRenderSceneRet = 0x3010EF35;
constexpr uintptr_t kRenderInnerCall = 0x3011106A; // RenderWorld's call of its inner pass ...
constexpr uintptr_t kRenderInnerFn = 0x3010AAC0;   // ... FUN_3010AAC0
constexpr uintptr_t kTerrainUnloadCall = 0x3010EF8F; // end-of-RenderScene garbage collection
constexpr uintptr_t kTerrainUnloadFn = 0x300A7C40;
constexpr uintptr_t kTerrainGlobal = 0x3022B8C0;     // Cry3DEngineBase::m_pTerrain, the call's argument (null without a level)
constexpr uintptr_t k3DEngineVtable = 0x301D37E8;    // C3DEngine: IProcess 0-5, LoadLevel 7, UnloadLevel 9, PostLoadLevel 10, OnFrameStart 11
constexpr uintptr_t kTmpPoolCall = 0x3010EFBD;
constexpr uintptr_t kTmpPoolFn = 0x300F4D80;
// Terrain. CTerrainNode::RenderSector (0x300C2E00), when the mesh is missing, the LOD changed or the vertex count
// differs, builds the sector vertices into the shared buffer (FUN_300BF320(node, step = edi, 0)) and calls
// UpdateRenderMesh(node, buffer + 0x130, force 1) at 0x300C3161; otherwise it calls UpdateRenderMesh(..., force 0) at
// 0x300C3139 without new vertices, which edits the existing mesh in place (indices, chunk array, chunks).
// step = min(g 0x3020049C << node+0xD0, (1 << node+0x68) * g 0x30200490).
constexpr uintptr_t kTerrainUpdateRenderMeshFn = 0x300C19A0;
// CTerrainNode::UpdateSurfaceRenderMeshes(sector mesh, info, CRenderMesh** surface slot, axis, index data, name): creates
// the surface (detail layer) mesh when the slot is empty, else edits it in place (vertex container, indices, chunks,
// material) and reads its chunks back. Called from RenderSector at 0x300C339D.
constexpr uintptr_t kTerrainSurfaceUpdateFn = 0x300C0A50;
constexpr uintptr_t kTerrainSurfaceCall = 0x300C339D;
constexpr uintptr_t kTerrainReuseCall = 0x300C3139;
constexpr uintptr_t kTerrainBuildVerticesFn = 0x300BF320;
constexpr uintptr_t kTerrainStepBase = 0x3020049C;
constexpr uintptr_t kTerrainUnitSize = 0x30200490;
constexpr uintptr_t kTerrainNodeLodShift = 0xD0; // int
constexpr uintptr_t kTerrainNodeLod = 0x68;      // uint8
// FUN_300BF050(node), called once, at the start of the sector draw FUN_300C12B0: the sector's texture ids (node +0x11C
// and +0x11E, ushort) into its mesh (node +0xC0 -> +0x40): CRenderMesh::SetMaterial(CTerrain+0x1D0 material, id) and
// straight into the first chunk's render element (+0x18: the id, CTerrain+0x24 or 0 by e_terrain_texture_debug at
// CVars+0xF4; +0x1C: the second id).
constexpr uintptr_t kTerrainTexturesFn = 0x300BF050;
constexpr uintptr_t kTerrainTexturesCall = 0x300C12C0;
constexpr uintptr_t kTerrainNodeTexture = 0x11C;
constexpr uintptr_t kTerrainNodeTexture2 = 0x11E;
constexpr uintptr_t k3DCVarsGlobal = 0x3022B8E0; // Cry3DEngineBase::m_pCVars
constexpr uintptr_t k3DEngineGlobal = 0x3022B8D8; // Cry3DEngineBase::m_p3DEngine
// C3DEngine::Update compiles one queued voxel object (terrain sculpting: cliffs, overhangs) and one queued road a
// frame: FUN_30141FE0(terrain) at 0x300F8143 takes the nearest of C3DEngine+0x8E8 (count +0x8F0) and rebuilds it
// (FUN_3014E050 "Compiling voxel object": frees the old mesh data at +0x160, clears layer slots at +0xA0);
// FUN_301415C0(terrain) at 0x300F814F compiles the first of +0x8F8 (count +0x900, CRoadRenderNode::Compile).
constexpr uintptr_t kVoxelCompileFn = 0x30141FE0, kVoxelCompileCall = 0x300F8143, kVoxelQueueCount = 0x8F0;
constexpr uintptr_t kRoadCompileFn = 0x301415C0, kRoadCompileCall = 0x300F814F, kRoadQueueCount = 0x900;
// CMatInfo (vtable 0x301DFAE8) reference count at +0x24, changed without a lock: AddRef 0x30152090 "add [rcx+0x24],1;
// ret" (padding after it), Release 0x301520A0 "sub rsp,28; add [rcx+0x24],-1; cmp [rcx+0x24],0; jg; call [vtable](1)"
// in 32 bytes. Only the vtable refers to them.
constexpr uintptr_t kMatAddRefFn = 0x30152090;
// CBaseResource AddRef as CShader slot 1 and CTexture slot 0 (mov eax,[rcx+0x10]; inc; store; returns the old count).
// Release is CBaseResource::Release 0x38049870 on the same count (+8 of the base at +8), deletes at zero.
constexpr uintptr_t kResourceAddRefFn = 0x38178CD0;
constexpr uintptr_t kObjectShaderEntries = 0x382E9B88; // pointer to the 0x14-byte entries (two per object, index at +0xD2)
constexpr uintptr_t kWaveFn = 0x3816F6D0;              // float (entry): evaluates an entry's wave form at the current time
constexpr uintptr_t kFadeFn = 0x38036D50; // float (renderer): fade factor of the current object (+0x84/+0x85, material flag 4)
constexpr uintptr_t kShaderReleaseFn = 0x38178CE0;  // CShader slot 2
constexpr uintptr_t kTextureReleaseFn = 0x380D43E0; // CTexture slot 1
constexpr uintptr_t kTextureGetFlagsFn = 0x380D4300; // CTexture slot 9: mov eax,[rcx+0xB8]
// C3DEngine::AddDynamicLightSource (FUN_300FD090, 0x752 bytes): copies the CDLight into the frame's light array, AddRefs
// the copy's shader (+0x1B0) and projected texture (+0x198) and releases the ones of the light it replaces.
constexpr uintptr_t kAddDynamicLightFn = 0x300FD090;
constexpr uintptr_t kAddDynamicLightSize = 0x752;
// CGameSerialize::SaveGame (CryAction FUN_3069E460) ends with the save object's Complete(true) (slot 7, CXmlSaveGame
// 0x306A7A70): the write (CXMLRichSaveGame slot 12 0x30791B40) and the object's deletion. The call sequence at
// 0x306A08FE: mov rax,[rbx]; mov dl,1; mov rcx,rbx; call [rax+0x38]; the result goes to edi at 0x306A091C.
constexpr uintptr_t kSaveCompleteSite = 0x306A08FE;
constexpr uint8_t kSaveCompleteCode[] = {0x48, 0x8B, 0x03, 0xB2, 0x01, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x38};
constexpr int kSaveCompleteSlot = 7;
constexpr int kFrameworkLoadGameSlot = 58; // CCryAction::LoadGame 0x30818E00 (IGameFramework vtable 0x3088C5E8)
constexpr uint8_t kMatAddRefCode[] = {0x83, 0x41, 0x24, 0x01, 0xC3, 0xCC};
constexpr uintptr_t kMatReleaseFn = 0x301520A0;
constexpr uint8_t kMatReleaseCode[] = {0x48, 0x83, 0xEC, 0x28, 0x83, 0x41, 0x24, 0xFF, 0x83, 0x79, 0x24, 0x00, 0x7F, 0x0A, 0x48, 0x8B,
                                       0x01, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x10, 0x48, 0x83, 0xC4, 0x28, 0xC3, 0xCC, 0xCC, 0xCC};
// Particles (_reference_target: Release is slot 2, the scalar deleting destructor slot 0).
constexpr uintptr_t kEmitterVtable = 0x301CE5F8;
constexpr uintptr_t kEmitterDeletingDtor = 0x300709B0;
constexpr uintptr_t kContainerVtable = 0x301CE758;
constexpr uintptr_t kContainerDeletingDtor = 0x30070450;

// ---------------------------------------------------------------------------------------------------------
// CryAnimation.dll
// ---------------------------------------------------------------------------------------------------------
constexpr uintptr_t kSkinRelease = 0x31526880;
constexpr uintptr_t kCharInstanceVtable = 0x316692A8;
constexpr uintptr_t kSkinInstanceVtable = 0x31669B58; // slot 30 clears the model pointer at +0xE8

// ---------------------------------------------------------------------------------------------------------
// CrySystem.dll
// ---------------------------------------------------------------------------------------------------------
constexpr uintptr_t kTimerVtable = 0x3676AC28; // CTimer, slot order of ITimer.h
} // namespace ce2
