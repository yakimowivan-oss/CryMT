// CryMT D3D12 back end, class Device: the GI settings (ini, panel) and the scene colour copy.
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // ---- queries
    void QueryBegin(Query12* q);
    void QueryEnd(Query12* q);
    HRESULT QueryData(Query12* q, void* data, UINT size, UINT flags);
    HRESULT QueryPeek(Query12* q, void* data, UINT size, UINT flags, uint64_t* flushSerial);
    HRESULT QueryRead(Query12* q, void* data, UINT size);
    void QueryFree(Query12* q);

    // ---- presentation
    HRESULT PresentFrame(IDXGISwapChain3* chain, ID3D12Resource* target, TextureCore* backbuffer, UINT sync, UINT flags);

    // ---- DLSS: the scene is complete, before HDR post-processing. The bound render target is the scene's HDR color
    // (RGBA16F) and the bound depth stencil its depth; DLAA's output replaces the color.
    // ---- CryMT GI, screen space (CRYMT_GI) ------------------------------------------------------------------------
    // At SceneDone the HDR scene colour is copied (the bounce source for the next frame) and the projection kept. The
    // next frame's first draw that reads the GI placeholder (general pass, after the z-prepass) runs a compute pass over
    // the scene depth: per half-resolution pixel, horizons along 4 screen directions (both ways) within a radius in
    // metres give the sky occlusion (a: share of the engine's flat ambient removed) and the light seen above the
    // horizon (rgb, cosine weighted), blended with the previous result. Pixel shaders then see this texture in place
    // of the placeholder. Own design after the horizon-based AO/indirect-light literature.
    struct GiSettings
    {
        bool on = false;
        float temporal = 0.75f, maxDistance = 120.0f, debug = 0.0f;
        bool upsample = true;    // CRYMT_GI_UPSAMPLE=0: the engine reads the half-size GI directly (v1.3)
        bool fixedNoise = false; // CRYMT_GI_FIXEDNOISE=1: the same sample pattern every frame
        bool nearest = false;    // CRYMT_GI_NEAREST=1: a GI texel stands for the nearest depth of its 2x2 block
        bool narrowNormals = false; // CRYMT_GI_NARROWNORMALS=1: normals one GI texel wide only (faces show on round meshes)
        bool weaponGi = true;       // CRYMT_GI_WEAPON=0: no GI on the first-person view (weapon, hands, held objects)
        float occFar = 16;          // CRYMT_GI_OCCRANGE (m): the cones' occlusion full to half of it, gone at it (0: no limit)
        float occStrength = 1;      // CRYMT_GI_OCCSTRENGTH: occlusion strength under cover (Num4 / Num7 multiply it)
        bool voxels = false;     // CRYMT_GI_VOXELS=1: the voxel grid around the camera (kVoxShader)
        float voxelSize = 0.25f; // CRYMT_GI_VOXELSIZE, metres (the finest level)
        float voxelView = 0;     // CRYMT_GI_VOXELVIEW: 1 wire faces over the frame + GI inset, 2 solid voxels (implies VOXELS)
        float voxelMix = 0;      // CRYMT_GI_VOXELS value: 1 the cones through the grid (the GI), 4 the grid without cones (cost)
        float voxelStrength = 1.5f, voxelReach = 64.0f; // CRYMT_GI_VOXELSTRENGTH (bounce light of the cones), CRYMT_GI_VOXELREACH (m)
        float voxelOcclusion = 0.6f; // CRYMT_GI_VOXELAO: how much of the flat ambient the cones' occlusion removes
        bool geometry = true;        // the z-prepass draws voxelized into level 0 (RunVoxelGeometry); CRYMT_GI_GEOMETRY=0 off
                                     // (default on since 2026-11-06, the user's choice: ~5-7 FPS)
        bool geometryFoliage = false; // CRYMT_GI_GEOMETRY=2: foliage too (mostly open cells)
        bool geometryPersist = true;  // CRYMT_GI_GEOMPERSIST=0: voxelized geometry only from the last three voxelizations
        bool opacityGeometry = true;  // the cells' opacity from the voxelized geometry only, the screen gives only their colour
                                      // (levels with geometry; the coarsest keeps the screen); CRYMT_GI_OCCUPANCY=screen: also
                                      // from the screen (before 2026-11-10: 12 x the cells coming and going on a still camera)
        float foliageOpacity = 0.5f;  // CRYMT_GI_FOLIAGE: an alpha-tested material's cell opacity (by leaf shape with materials)
        bool materials = true;        // CRYMT_GI_MATERIALS=0: geometry without textures (no albedo, no alpha test)
        float coneLift = 0.5f, coneStart = 1; // CRYMT_GI_CONELIFT, CRYMT_GI_CONESTART: the near cones', cells of level 0
        float nearOcclusion = 0;           // CRYMT_GI_NEAROCC: strength of the near occlusion cones (0 none; Num3 sets 1)
        float sunAlbedo = 0.3f;            // CRYMT_GI_SUNALBEDO: the albedo assumed for sunlit geometry cells (0: none)
        float skyFill = 0.15f;             // CRYMT_GI_SKYFILL: their light from the sky, a share of the sun's
        int sunMarch = 128;                // CRYMT_GI_SUNMARCH: cells the light pass marches towards the sun in a level, up to
                                           // 128 (the grid). 24 before 2026-11-13 (6 m at level 0): mine at noon, the light
                                           // change of cells coming into view (inside, level 1) mean 12.9 -> 4.7 / 255, > 48:
                                           // 16% -> 2% of cells; +0.05 ms
        bool shadowGeometry = false;       // CRYMT_GI_SHADOWGEOM=1: also the shadow maps' casters (off: their vertex shaders
                                           // put the depth elsewhere, none of their draws landed in the grid - a capture)
        bool cubeGeometry = false;         // CRYMT_GI_CUBEGEOM=1: the engine cube's face draws (solid only) into the geometry
                                           // levels, their stamps to fill what the camera does not see. OFF: it does not work
                                           // (2026-11-13, mine): the general pass places objects by a per-object matrix that
                                           // already holds the camera, so replacing the frame's view-projection left them in the
                                           // face's perspective - streaks through the whole grid (voxsnap_views.py)
        bool cubeInject = false;           // CRYMT_GI_CUBEINJECT=1: cells from the engine cube's faces (kVoxShader "inject": the
                                           // surfaces each face's depth shows, with its lit colour, outside the view). Off: on
                                           // mine (2026-11-13, noon) it added only 3-5k level-0 cells, the cells found on
                                           // turning were 93-98% surfaces the cube cannot see (backs, hidden parts), and
                                           // inside the light change on turning grew (> 48/255: 14% of cells vs 1.4%)
        float lampLight = 1.0f;            // CRYMT_GI_LAMPS: the lamps' light on the geometry's cells (0 none)
        bool cellAxes = false;             // CRYMT_GI_CELLAXES=1: a cone's own surface test by the cell read, not the cone's width
                                           // (mine yard 2026-11-15: occlusion -0.01..-0.04, light up to x2: mixed, off)
        bool farProbes = true;             // CRYMT_GI_FARPROBES=0: the cones' occlusion everywhere (with the probes past 32 m:
                                           // their occlusion, no cones traced there)
        bool skyOcclusion = false;         // CRYMT_GI_SKYOCC=1: the cones' occlusion from their part above the horizon only (the
                                           // engine's ambient is the sky; the ground below is bounce, not lost sky)
        bool probeFast = true;             // CRYMT_GI_PROBEFAST=0: GI v2's probes at their fixed pace (no faster response to a
                                           // change beyond their rays' noise: kGiShader "probes")
        bool lampTest = false;             // CRYMT_GI_LAMPTEST=1: also on cells the screen coloured (to see it in the voxel view)
        int mipOpacity = 4;                // CRYMT_GI_MIPOPACITY: coarse cells' opacity, 0 the mean, 1/2/3 by rays through, 4 per axis (downsample)
        float ambientFloor = 0.1f;         // CRYMT_GI_FLOOR: the share of the flat ambient full occlusion keeps (Strengthen)
        float outdoorStrength = 0.7f;      // CRYMT_GI_OUTDOOR: the occlusion strength outdoors (Num4 / Num7 set the indoor one);
                                           // 1.0 before 2026-11-14 (with the voxels' occlusion 1.2 the cones darkened open
                                           // yards x0.15-0.2 against no GI; the indoor strength keeps the rooms dark), then
                                           // 0.45 (forest lighter than no GI), 0.7 the user's pick (forest 44.7, no GI 49.9)
        bool roofCells = false;            // CRYMT_GI_ROOFCELLS=1: the roof texture (kVoxShader "roof") as well as the ray
        bool thinFix = true;               // CRYMT_GI_THIN=0: thin features keep their own (chance) GI
        bool roomLight = true;             // CRYMT_GI_ROOMLIGHT=0: cones from inside a VisArea take light from outside it too
                                           // (2026-11-13: sunlit roofs and outer walls leaked in through the coarse mips)
        int dirLight = 1;                  // CRYMT_GI_DIRLIGHT: the cells' light, 0 one colour for all directions, 1 a colour
                                           // and the direction it goes out stronger (L1: a thin wall lit outside stays dark
                                           // inside; the voxels page; default since 2026-11-14, the user), 2 six directions
                                           // (+-X, +-Y, +-Z, ~220 MB more, made when first asked for)
        int probes = 0;                    // CRYMT_GI_PROBES=1: GI v2, irradiance probes traced through the voxels in place
                                           // of the cones near the camera (kGiShader "probes", ProbeLight); 2: the probes'
                                           // light with the cones' occlusion
        bool probeVisibility = true;       // CRYMT_GI_PROBEVIS=0: no visibility test (measures the light it keeps out)
        float probeStrength = 1.5f;        // CRYMT_GI_PROBESTRENGTH: their bounce light's factor (the cones' is voxelStrength
                                           // 1.5: coarse mips lose light; the probes' thin rays see the cells' full light).
                                           // 1.0 until 2026-11-14 (at 1.5 the loop through the screen's colour once lit
                                           // closed rooms by itself, before the probes' fixes); 1.5: the user's +50%
        float gtaoRadius = 0.5f;          // CRYMT_GI_GTAORADIUS, metres
        bool contactShadows = true;        // CRYMT_CONTACTSHADOWS=0: no screen-space contact shadows in the sun's shadow mask
        float contactLength = 0.35f;       // CRYMT_CONTACTLENGTH: the march's length at 10 m (metres; scales with distance)
        bool contactFoliage = true;        // CRYMT_CONTACTFOLIAGE=0: foliage (the z-prepass mask) casts no contact shadows (default on since 2026-11-10, the user)
        bool sss = true;                   // CRYMT_SSS=0: the old contact shadows (12 noisy steps in the shadow mask) in place of
                                           // Bend Studio's screen-space shadows (RunSss, BendSss.h)
        int sssSamples = 60;               // CRYMT_SSS_SAMPLES: their length in screen pixels (Bend: 60; compiled in)
        float sssThickness = 0.005f;       // CRYMT_SSS_THICKNESS: a pixel's assumed thickness, a share of its depth (Bend: 0.005)
        float sssContrast = 4.0f;          // CRYMT_SSS_CONTRAST (Bend: 2..4)
        float sssEdge = 0.02f;             // CRYMT_SSS_EDGE: a depth step taken as an edge, a share of the depth (Bend: 0.02)
        float sssReach = 4000.0f;          // CRYMT_SSS_REACH: metres from the camera; faded over the last quarter (the
                                           // materials take them beyond the shadow maps too: fragLib, Terrain). 4000 m
                                           // since 2026-11-12 (the user; 0.235 ms at 5120x1440 against 0.231 at 1000 m)
        float sssNear = 0.0f;              // CRYMT_SSS_NEAR: metres; nearer pixels get none
        float sssWeaponThickness = 0.1f;   // CRYMT_SSS_WEAPONTHICK: the first-person view's thickness, a share of its depth (at
                                           // the world's 0.005 the weapon cast no shadow on the hands)
        float gtao = 1.5f;                 // CRYMT_GI_GTAO=<strength>: GTAO in place of the engine's SSAO, a factor on the GI's
                                           // result as the SSAO was (village rooms: darkening 41.8/68.7/48.2% at 1.5 against the
                                           // SSAO's 44.1/69.8/54.2%; creases darker than the SSAO, open walls not darkened)
        bool replaceSsao = true;           // CRYMT_GI_SSAO=1: keep the engine's SSAO (on top of the GI's occlusion: it doubled
                                           // it; without GTAO the creases came out light, the user saw light in the corners)
        int bakedOff = 30;                 // the baked occlusion the GI replaces near the camera, bits 2 the objects' RAM, 4 their
                                           // sky amount, 8 TerrainAO (under trees), 16 the terrain's vertex AO: by default all
                                           // (the user: our GI near, the baked further, in full far away); CRYMT_GI_BAKED=engine
                                           // none (the engine's everywhere, under the GI), or a list (ram,sky,terrainao,vertex)
        float terrainAoNear = 62.0f;       // CRYMT_GI_TERRAINAONEAR: metres up to which the GI alone works; over 1.5 x that
                                           // the GI fades out and the baked terms in (far trees are sprites, beyond the voxels);
                                           // 62 (the panel's top, "up to 155") since 2026-11-14, the user (30 before)
        bool gtaoFull = false;             // CRYMT_GI_GTAOFULL=1: GTAO at full size (island 5120x1440: 3.5 ms against 1.2)
        bool gtaoNoFoliage = false;        // CRYMT_GI_GTAOFOLIAGE=0: grass and leaves (the foliage mask) occlude nothing in GTAO
                                           // (the user: plants looked worse without their occlusion)
        float foliageBounce = 0.5f;        // CRYMT_GI_FOLIAGEBOUNCE: the bounce light on leaves times this (their occlusion
                                           // keeps CRYMT_GI_FOLIAGEGI); 0.5 with the bounce +50% overall = half of before
                                           // (the user, 2026-11-14)
        float grassGi = 1.0f;              // CRYMT_GI_GRASSGI: the same for grass (marked apart by the z-prepass), which keeps
                                           // the engine's SSAO in place of GTAO (2026-11-10, the user: grass flat, too bright)
        float foliageGi = 1.5f;            // CRYMT_GI_FOLIAGEGI: the GI's occlusion and bounce on grass and leaves (the foliage
                                           // mask) times this (the user: more of the voxel GI on grass); 1 as everything else
        bool particlesGi = true;           // CRYMT_GI_PARTICLES=0 (key "-"): blended particles (smoke, dust) without the GI
        int vfog = 1;                      // CRYMT_VFOG: the GI in the engine's fog (RunVfog, the patched FogPass): 0 off, 1
                                           // on, 2 the volume made but zero (must give the engine's fog exactly: a check)
        float vfogGi = 1.0f;               // CRYMT_VFOG_GI: its strength (the particles' ambient share lost, times this)
        float vfogSun = 1.0f;              // CRYMT_VFOG_SUN: the sun's shadow maps in the fog (stage 2: its haze dark where the
                                           // cascades see shade, outdoors too); 0 off (stage 1: the voxels' sun indoors)
        float vfogHaze = 0.0f;             // CRYMT_VFOG_HAZE: the near air - the share of light a haze of its own takes over
                                           // its reach (0 none; the engine's fog near the camera is thin, the sun's shafts
                                           // hardly show without it; the user's panel row "Плотность воздуха вблизи")
        float vfogHazeReach = 60.0f;       // CRYMT_VFOG_HAZEREACH: its reach (m): full density to half of it, none at it
        int vfogSamples = 8;               // CRYMT_VFOG_SAMPLES: the sun's shadow samples per cell, 1 / 4 / 8 (one drew the
                                           // shadows' edges in the fog as steps of whole tiles: the user, Sponza 2026-11-15;
                                           // 8 by default, the user)
        float vfogOutdoor = 1.0f;          // CRYMT_VFOG_OUT: its darkening outdoors against indoors (the fog's own; the
                                           // particles' 200% made the path under palms too dark, the user 2026-11-14)
        float particleCell = 1.5f;         // CRYMT_GI_PARTICLECELL: their coarse occlusion volume's cell (m; 64 x 64 x 32 cells)
        float particleFineCell = 0.5f;     // CRYMT_GI_PARTICLEFINE: the fine one's (rooms' walls and ceilings)
        float particleOutdoor = 2.0f;      // CRYMT_GI_PARTICLEOUT: the darkening outdoors against indoors (key "=" 50 / 70 / 100 /
                                           // 150 / 200%; the user wanted more than 100%; thin particles keep more of their light)
        float particleMax = 0.85f;         // CRYMT_GI_PARTICLEMAX: at most this share of a particle's ambient goes
        float particleTint = 0.5f;         // CRYMT_GI_PARTICLETINT: their ambient takes this much of the voxels' light hue
        bool gtaoFoliageShade = true;      // CRYMT_GI_GTAOFOLIAGESHADE=0: GTAO at full strength also on foliage in the sun's
                                           // shadow (the user: there it was too strong; in sunlight fine): 0.2 there
        bool gtaoHistory = false;          // CRYMT_GI_GTAOHISTORY=1: its own history (0.9, reprojected): it swam on camera
                                           // turns; without, the slice turns move per frame and DLAA accumulates them
        float gtaoHistoryWeight = 0.9f;    // ... the history's weight: CRYMT_GI_GTAOHISTORY=<under 1> or the AO page's
                                           // "Накопление GTAO" (the user, 2026-11-14: GTAO noisy on small objects and
                                           // leaves while moving; 0.9 swam)
    };
    static const GiSettings& GiBase() // the ini / environment values
    {
        static const GiSettings s = [] {
            GiSettings v;
            wchar_t text[32] = {};
            v.on = GetEnvironmentVariableW(L"CRYMT_GI", text, 32) && wcscmp(text, L"0") != 0;
            const auto read = [](const wchar_t* name, float& value) {
                wchar_t t[32] = {};
                if (GetEnvironmentVariableW(name, t, 32))
                    value = static_cast<float>(_wtof(t));
            };
            read(L"CRYMT_GI_TEMPORAL", v.temporal);
            read(L"CRYMT_GI_DISTANCE", v.maxDistance);
            read(L"CRYMT_GI_DEBUG", v.debug);
            v.upsample = !(GetEnvironmentVariableW(L"CRYMT_GI_UPSAMPLE", text, 32) && wcscmp(text, L"0") == 0);
            v.fixedNoise = GetEnvironmentVariableW(L"CRYMT_GI_FIXEDNOISE", text, 32) && wcscmp(text, L"0") != 0;
            v.nearest = GetEnvironmentVariableW(L"CRYMT_GI_NEAREST", text, 32) && wcscmp(text, L"0") != 0;
            v.narrowNormals = GetEnvironmentVariableW(L"CRYMT_GI_NARROWNORMALS", text, 32) && wcscmp(text, L"0") != 0;
            v.weaponGi = !(GetEnvironmentVariableW(L"CRYMT_GI_WEAPON", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_OCCRANGE", v.occFar);
            read(L"CRYMT_GI_OCCSTRENGTH", v.occStrength);
            // The voxel cones are the GI; CRYMT_GI_VOXELS=0: no grid (no GI, to measure), 4 the grid without cones.
            const bool voxelVar = GetEnvironmentVariableW(L"CRYMT_GI_VOXELS", text, 32) != 0;
            v.voxels = voxelVar ? wcscmp(text, L"0") != 0 : v.on;
            v.voxelMix = !v.voxels ? 0.0f : voxelVar && _wtof(text) == 4 ? 4.0f : 1.0f;
            read(L"CRYMT_GI_VOXELSTRENGTH", v.voxelStrength);
            read(L"CRYMT_GI_VOXELREACH", v.voxelReach);
            read(L"CRYMT_GI_VOXELAO", v.voxelOcclusion);
            read(L"CRYMT_GI_VOXELSIZE", v.voxelSize);
            read(L"CRYMT_GI_VOXELVIEW", v.voxelView);
            v.voxels = v.voxels || v.voxelView > 0;
            const bool geometryVar = GetEnvironmentVariableW(L"CRYMT_GI_GEOMETRY", text, 32) != 0;
            v.geometry = v.voxels && !(geometryVar && wcscmp(text, L"0") == 0);
            v.geometryFoliage = v.geometry && geometryVar && wcscmp(text, L"2") == 0;
            v.geometryPersist = !(GetEnvironmentVariableW(L"CRYMT_GI_GEOMPERSIST", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_FOLIAGE", v.foliageOpacity);
            v.opacityGeometry = !(GetEnvironmentVariableW(L"CRYMT_GI_OCCUPANCY", text, 32) && _wcsicmp(text, L"screen") == 0);
            v.materials = !(GetEnvironmentVariableW(L"CRYMT_GI_MATERIALS", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_GI_CONELIFT", v.coneLift);
            read(L"CRYMT_GI_CONESTART", v.coneStart);
            read(L"CRYMT_GI_NEAROCC", v.nearOcclusion);
            read(L"CRYMT_GI_SUNALBEDO", v.sunAlbedo);
            read(L"CRYMT_GI_SKYFILL", v.skyFill);
            {
                float steps = static_cast<float>(v.sunMarch);
                read(L"CRYMT_GI_SUNMARCH", steps);
                v.sunMarch = std::min(std::max(static_cast<int>(steps), 1), 128);
            }
            v.shadowGeometry = GetEnvironmentVariableW(L"CRYMT_GI_SHADOWGEOM", text, 32) && wcscmp(text, L"0") != 0;
            v.cubeGeometry = GetEnvironmentVariableW(L"CRYMT_GI_CUBEGEOM", text, 32) && wcscmp(text, L"0") != 0;
            v.cubeInject = GetEnvironmentVariableW(L"CRYMT_GI_CUBEINJECT", text, 32) && wcscmp(text, L"0") != 0;
            read(L"CRYMT_GI_LAMPS", v.lampLight);
            v.probeFast = !(GetEnvironmentVariableW(L"CRYMT_GI_PROBEFAST", text, 32) && wcscmp(text, L"0") == 0);
            v.skyOcclusion = GetEnvironmentVariableW(L"CRYMT_GI_SKYOCC", text, 32) && wcscmp(text, L"0") != 0;
            v.cellAxes = GetEnvironmentVariableW(L"CRYMT_GI_CELLAXES", text, 32) && wcscmp(text, L"0") != 0;
            v.farProbes = !(GetEnvironmentVariableW(L"CRYMT_GI_FARPROBES", text, 32) && wcscmp(text, L"0") == 0);
            v.lampTest = GetEnvironmentVariableW(L"CRYMT_GI_LAMPTEST", text, 32) && wcscmp(text, L"0") != 0;
            if (GetEnvironmentVariableW(L"CRYMT_GI_MIPOPACITY", text, 32))
                v.mipOpacity = _wtoi(text);
            read(L"CRYMT_GI_FLOOR", v.ambientFloor);
            read(L"CRYMT_GI_OUTDOOR", v.outdoorStrength);
            v.roofCells = GetEnvironmentVariableW(L"CRYMT_GI_ROOFCELLS", text, 32) && wcscmp(text, L"0") != 0;
            v.thinFix = !(GetEnvironmentVariableW(L"CRYMT_GI_THIN", text, 32) && wcscmp(text, L"0") == 0);
            v.roomLight = !(GetEnvironmentVariableW(L"CRYMT_GI_ROOMLIGHT", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_GI_DIRLIGHT", text, 32))
                v.dirLight = std::min(std::max(_wtoi(text), 0), 2);
            if (GetEnvironmentVariableW(L"CRYMT_GI_PROBES", text, 32))
                v.probes = std::min(std::max(_wtoi(text), 0), 2);
            read(L"CRYMT_GI_PROBESTRENGTH", v.probeStrength);
            v.probeVisibility = !(GetEnvironmentVariableW(L"CRYMT_GI_PROBEVIS", text, 32) && wcscmp(text, L"0") == 0);
            v.replaceSsao = !(GetEnvironmentVariableW(L"CRYMT_GI_SSAO", text, 32) && wcscmp(text, L"0") != 0);
            read(L"CRYMT_GI_GTAO", v.gtao);
            read(L"CRYMT_GI_GTAORADIUS", v.gtaoRadius);
            v.contactShadows = !(GetEnvironmentVariableW(L"CRYMT_CONTACTSHADOWS", text, 32) && wcscmp(text, L"0") == 0);
            read(L"CRYMT_CONTACTLENGTH", v.contactLength);
            v.contactFoliage = !(GetEnvironmentVariableW(L"CRYMT_CONTACTFOLIAGE", text, 32) && wcscmp(text, L"0") == 0);
            v.sss = !(GetEnvironmentVariableW(L"CRYMT_SSS", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_SSS_SAMPLES", text, 32))
                v.sssSamples = std::min(std::max(_wtoi(text), 16), 256);
            read(L"CRYMT_SSS_THICKNESS", v.sssThickness);
            read(L"CRYMT_SSS_CONTRAST", v.sssContrast);
            read(L"CRYMT_SSS_EDGE", v.sssEdge);
            read(L"CRYMT_SSS_REACH", v.sssReach);
            read(L"CRYMT_SSS_NEAR", v.sssNear);
            read(L"CRYMT_SSS_WEAPONTHICK", v.sssWeaponThickness);
            if (GetEnvironmentVariableW(L"CRYMT_GI_BAKED", text, 32))
            {
                _wcslwr_s(text);
                v.bakedOff = wcscmp(text, L"0") == 0 ? 30 : wcscmp(text, L"engine") == 0 ? 0
                                                     : (wcsstr(text, L"ram") ? 2 : 0) | (wcsstr(text, L"sky") ? 4 : 0) | (wcsstr(text, L"terrainao") ? 8 : 0) |
                                                           (wcsstr(text, L"vertex") ? 16 : 0);
            }
            read(L"CRYMT_GI_TERRAINAONEAR", v.terrainAoNear);
            v.gtaoFull = GetEnvironmentVariableW(L"CRYMT_GI_GTAOFULL", text, 32) && wcscmp(text, L"0") != 0;
            v.gtaoHistory = GetEnvironmentVariableW(L"CRYMT_GI_GTAOHISTORY", text, 32) && wcscmp(text, L"0") != 0;
            if (v.gtaoHistory)
            {
                const float w = static_cast<float>(_wtof(text));
                v.gtaoHistoryWeight = w > 0.0f && w < 1.0f ? w : 0.9f; // "1": the old 0.9
            }
            v.gtaoNoFoliage = GetEnvironmentVariableW(L"CRYMT_GI_GTAOFOLIAGE", text, 32) && wcscmp(text, L"0") == 0;
            read(L"CRYMT_GI_FOLIAGEGI", v.foliageGi);
            read(L"CRYMT_GI_FOLIAGEBOUNCE", v.foliageBounce);
            read(L"CRYMT_GI_GRASSGI", v.grassGi);
            v.particlesGi = !(GetEnvironmentVariableW(L"CRYMT_GI_PARTICLES", text, 32) && wcscmp(text, L"0") == 0);
            if (GetEnvironmentVariableW(L"CRYMT_VFOG", text, 32))
                v.vfog = std::min(std::max(_wtoi(text), 0), 2);
            read(L"CRYMT_VFOG_GI", v.vfogGi);
            v.vfogGi = std::min(std::max(v.vfogGi, 0.0f), 3.0f);
            read(L"CRYMT_VFOG_SUN", v.vfogSun);
            v.vfogSun = std::min(std::max(v.vfogSun, 0.0f), 1.0f);
            read(L"CRYMT_VFOG_HAZE", v.vfogHaze);
            v.vfogHaze = std::min(std::max(v.vfogHaze, 0.0f), 0.9f);
            if (GetEnvironmentVariableW(L"CRYMT_VFOG_SAMPLES", text, 32))
                v.vfogSamples = _wtoi(text) >= 8 ? 8 : _wtoi(text) >= 4 ? 4 : 1;
            read(L"CRYMT_VFOG_HAZEREACH", v.vfogHazeReach);
            v.vfogHazeReach = std::min(std::max(v.vfogHazeReach, 5.0f), 128.0f);
            read(L"CRYMT_VFOG_OUT", v.vfogOutdoor);
            v.vfogOutdoor = std::min(std::max(v.vfogOutdoor, 0.0f), 3.0f);
            read(L"CRYMT_GI_PARTICLECELL", v.particleCell);
            v.particleCell = v.particleCell > 0.25f ? v.particleCell : 1.5f;
            read(L"CRYMT_GI_PARTICLEFINE", v.particleFineCell);
            v.particleFineCell = v.particleFineCell > 0.1f ? v.particleFineCell : 0.5f;
            read(L"CRYMT_GI_PARTICLEOUT", v.particleOutdoor);
            read(L"CRYMT_GI_PARTICLEMAX", v.particleMax);
            read(L"CRYMT_GI_PARTICLETINT", v.particleTint);
            v.gtaoFoliageShade = !(GetEnvironmentVariableW(L"CRYMT_GI_GTAOFOLIAGESHADE", text, 32) && wcscmp(text, L"0") == 0);
            v.voxelSize = v.voxelSize > 0.05f ? v.voxelSize : 0.25f;
            return v;
        }();
        return s;
    }

    // The live values: the ini's, with the GI test keys' switches applied at present (stream thread, as every reader).
    static GiSettings& Gi()
    {
        static GiSettings live = GiBase();
        return live;
    }

    // The contact shadows' mode as the ini says: 0 off, 1 the old ones, 2 Bend's screen-space shadows.
    static int BaseContact()
    {
        const GiSettings& b = GiBase();
        return b.contactShadows ? (b.sss ? 2 : 1) : 0;
    }

    void GiApplyControl()
    {
        static const bool fixCopiesEnv = [] { // CRYMT_DLSS_FIXCOPIES=0: off from the start (key "]")
            wchar_t text[8] = {};
            if (GetEnvironmentVariableW(L"CRYMT_DLSS_FIXCOPIES", text, 8) && wcscmp(text, L"0") == 0)
                g_giControl.fixCopies = 0;
            return true;
        }();
        (void)fixCopiesEnv;
        if (!g_giControl.active)
            return;
        const GiSettings& b = GiBase();
        GiSettings& g = Gi();
        const float occlusion = g_giControl.occlusionScale, bounce = g_giControl.bounceScale;
        const bool wasOn = g.on;
        g.on = b.on && g_giControl.enabled;
        if (g.on && !wasOn)
            m_giReset = true; // the history is from before
        if (!g.on && !m_giSrv)
            m_giCpu = {}; // shaders see the placeholder: no GI (CRYMT_GITEST keeps its texture)
        g.occStrength = b.occStrength * occlusion; // Num4 / Num7: where there is cover above (Strengthen), not everywhere
        g.voxelStrength = b.voxelStrength * bounce;
        g.probeStrength = b.probeStrength * bounce; // GI v2 too (the user liked Sponza's strong bounce)
        g.voxels = b.on; // the grid keeps up, so the cones and the view switch on at once
        g.voxelMix = b.voxelMix == 4 ? 4.0f : 1.0f; // the cones (as the grid, they keep up with GI on)
        g.weaponGi = b.weaponGi && g_giControl.weapon;
        g.voxelView = static_cast<float>(g_giControl.view.load());
        // Num3: the voxel geometry with the near occlusion cones; off gives the ini's settings exactly.
        if (g_giControl.geometry < 0)
            g_giControl.geometry = b.geometry ? (b.geometryFoliage ? 2 : 1) : 0;
        const int geometry = g_giControl.geometry;
        g.geometry = b.voxels && geometry > 0;
        g.geometryFoliage = geometry == 2;
        g.nearOcclusion = geometry > 0 ? (b.nearOcclusion > 0 ? b.nearOcclusion : 1.0f) : 0.0f;
        // Voxels page Num0: where the cells' opacity comes from.
        if (g_giControl.voxOpacity < 0)
            g_giControl.voxOpacity = b.opacityGeometry ? 1 : 0;
        g.opacityGeometry = g_giControl.voxOpacity == 1 && g.geometry;
        // Voxels page Num1: unchanged objects every 4th frame or every frame.
        // PgDn: GTAO in place of the engine's SSAO, or the SSAO; Home: contact shadows.
        if (g_giControl.gtao < 0)
            g_giControl.gtao = b.replaceSsao ? 1 : 0;
        g.replaceSsao = g_giControl.gtao > 0;
        if (g_giControl.contact < 0)
            g_giControl.contact = BaseContact();
        g.contactShadows = g_giControl.contact > 0;
        g.sss = g_giControl.contact == 2;
        g.sssReach = g_giControl.sssReach > 0 ? static_cast<float>(g_giControl.sssReach) : b.sssReach;
        g.sssWeaponThickness = g_giControl.sssWeapon >= 0 && g_giControl.sssWeapon < 5 ? kSssWeaponThick[g_giControl.sssWeapon] : b.sssWeaponThickness;
        g.terrainAoNear = g_giControl.giNear >= 0 && g_giControl.giNear < 4 ? static_cast<float>(kGiNears[g_giControl.giNear]) : b.terrainAoNear;
        g.dirLight = g_giControl.dirLight >= 0 ? std::min(g_giControl.dirLight.load(), 2) : b.dirLight;
        g.probes = g_giControl.probes >= 0 ? std::min(g_giControl.probes.load(), 2) : b.probes;
        g.probeFast = g_giControl.probeFast >= 0 ? g_giControl.probeFast != 0 : b.probeFast;
        g.farProbes = g_giControl.farProbes >= 0 ? g_giControl.farProbes != 0 : b.farProbes;
        g.skyOcclusion = g_giControl.skyOcc >= 0 ? g_giControl.skyOcc != 0 : b.skyOcclusion;
        g.cellAxes = g_giControl.cellAxes >= 0 ? g_giControl.cellAxes != 0 : b.cellAxes;
        if (g_giControl.gtaoHistory >= 0) // the AO page's "Накопление GTAO": off, 0.5, 0.7, 0.9
        {
            static constexpr float kWeights[4] = {0.0f, 0.5f, 0.7f, 0.9f};
            const int i = std::min(g_giControl.gtaoHistory.load(), 3);
            g.gtaoHistory = i > 0;
            g.gtaoHistoryWeight = i > 0 ? kWeights[i] : b.gtaoHistoryWeight;
        }
        if (g_giControl.particles < 0)
            g_giControl.particles = b.particlesGi ? 1 : 0;
        g.particlesGi = g_giControl.particles > 0;
        if (g_giControl.vfog < 0)
            g_giControl.vfog = b.vfog;
        g.vfog = std::min(g_giControl.vfog.load(), 2);
        g.vfogGi = g_giControl.vfogGi >= 0 ? g_giControl.vfogGi / 100.0f : b.vfogGi;
        g.vfogOutdoor = g_giControl.vfogOut >= 0 ? g_giControl.vfogOut / 100.0f : b.vfogOutdoor;
        g.vfogSun = g_giControl.vfogSun >= 0 ? std::min(g_giControl.vfogSun / 100.0f, 1.0f) : b.vfogSun;
        g.vfogHaze = g_giControl.vfogHaze >= 0 ? std::min(g_giControl.vfogHaze / 100.0f, 0.9f) : b.vfogHaze;
        g.vfogSamples = g_giControl.vfogSamples >= 0 ? (g_giControl.vfogSamples >= 8 ? 8 : g_giControl.vfogSamples >= 4 ? 4 : 1) : b.vfogSamples;
        if (g_giControl.particleOut < 0)
            g_giControl.particleOut = static_cast<int>(b.particleOutdoor * 100.0f + 0.5f);
        g.particleOutdoor = g_giControl.particleOut / 100.0f;
        if (g_giControl.bakedOff < 0)
            g_giControl.bakedOff = b.bakedOff;
        g.bakedOff = g_giControl.bakedOff & 30;
    }

    // A null state is D3D10's default rasterizer: solid. The editor's wireframe views share the main renderer and the
    // scene's targets; they must not become the GI's source nor run its pass (GetMainViewport.patch, 2026-11-14).
    bool GiSolidFill() const { return !m_raster || m_raster->desc.FillMode == D3D10_FILL_SOLID; }

    void GiSceneDone(const DlssSceneInfo* info)
    {
        if (!Gi().on || !GiSolidFill() || !info || !info->valid || !m_rtv[0] || !m_dsv)
            return;
        auto* color = static_cast<TextureCore*>(m_rtv[0]->info);
        auto* depth = static_cast<TextureCore*>(m_dsv->info);
        if (depth->width < color->width || depth->height < color->height || color->samples != 1 || depth->samples != 1 ||
            !EnsureGi(color->width, color->height, color->format))
            return;
        m_giSceneDepth = m_dsv->info;
        memcpy(m_giSun, info->sun, sizeof(m_giSun));
        memcpy(m_giLamps, info->lamps, sizeof(m_giLamps));
        m_giLampCount = std::min<uint32_t>(info->lampCount, 8);
        memcpy(m_giVisAreas, info->visAreas, sizeof(m_giVisAreas));
        m_giVisAreaCount = std::min<uint32_t>(info->visAreaCount, 16);
        if (m_rtv[0]->info != m_giSceneColor)
        {
            m_giSceneColorPrev = m_giSceneColor;
            m_giSceneColor = m_rtv[0]->info;
        }
        Require(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_barriers.push_back(Transition(m_giColor, kSrvState, D3D12_RESOURCE_STATE_COPY_DEST));
        FlushBarriers();
        m_list->CopyResource(m_giColor, color->res);
        m_barriers.push_back(Transition(m_giColor, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
        m_giColorFrame = m_frame;
        m_giFrame++;
    }
