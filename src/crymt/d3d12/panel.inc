// CryMT D3D12 back end, class Device: the in-game panel (Num/).
// Included inside the class in D3D12Backend.cpp, where this text stood.

    // The panel (CRYMT_GI_KEYS=1, the launcher's in-game keys): tabs of live settings drawn with GDI (Consolas, bold,
    // about 1/60 of the screen height) into a texture when they change, at the left, centred in height. Num/ shows or
    // hides it, Num7 / Num9 the tab, Num8 / Num2 the row, Num4 / Num6 its value -/+ (held: repeats), Num5 its default;
    // Num+ / Num- move the time of day on any tab (CryMT.cpp). A value off its default (the ini's, CryMT's) is orange
    // with a star. One PanelItem per setting: its value an int in lo..hi (toggles 0..1, lists index `names`, scales
    // count steps of `scale`); get / def / set read and write g_giControl (set runs on main, on the keys;
    // GiApplyControl applies the values at present).
    struct PanelItem
    {
        int tab;
        const char* label; // "русский|English"
        int lo, hi, step;
        bool wrap;                // lists and toggles go round, scales stop at their ends
        const char* const* names; // per value "русский|English", or null: the value times `scale` by `format`
        float scale;
        const char* format;
        int (*get)();
        int (*def)();
        void (*set)(int);
        bool mark;                         // highlighted off its default (not the time of day)
        std::string (*text)(bool ru);      // its own text, or null
    };
    static constexpr int kPanelTabs = 6;
    static std::string PanelLang(const char* s, bool ru)
    {
        const char* bar = strchr(s, '|');
        return !bar ? std::string(s) : ru ? std::string(s, bar - s) : std::string(bar + 1);
    }
    static bool PanelRussian()
    {
        static const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        return ru;
    }
    static const char* PanelTabName(int t)
    {
        static const char* const names[kPanelTabs] = {"GI|GI", "AO|AO", "Воксели|Voxels", "Отражения|Reflections", "Движок|Engine", "Время|Time"};
        return names[t];
    }
    static bool EnvDefaultOn() { return EnvModeBase() == 1; }
    static int BaseParticleOut()
    {
        static const int values[5] = {50, 70, 100, 150, 200};
        const int pct = static_cast<int>(GiBase().particleOutdoor * 100.0f + 0.5f);
        int best = 0;
        for (int i = 1; i < 5; i++)
            if (abs(values[i] - pct) < abs(values[best] - pct))
                best = i;
        return best;
    }
    // The screen-space shadows' reach on the panel (metres): the list's entry nearest a value.
    static constexpr int kSssReaches[6] = {100, 200, 500, 1000, 2000, 4000};
    static constexpr float kSssWeaponThick[5] = {0.005f, 0.02f, 0.05f, 0.1f, 0.2f}; // the panel's list for the weapon and hands
    static constexpr int kGiNears[4] = {30, 40, 50, 62}; // the panel's "GI at full strength up to" (metres; 62: the field's top)
    // The AO page's "Накопление GTAO" index for the ini / environment value: 0 off, 1 0.5, 2 0.7, 3 0.9 (the nearest).
    static int GtaoHistoryIndex(const GiSettings& s)
    {
        if (!s.gtaoHistory)
            return 0;
        return s.gtaoHistoryWeight < 0.6f ? 1 : s.gtaoHistoryWeight < 0.8f ? 2 : 3;
    }
    static int GiNearIndex(float metres)
    {
        int best = 0;
        for (int i = 1; i < 4; i++)
            if (fabsf(static_cast<float>(kGiNears[i]) - metres) < fabsf(static_cast<float>(kGiNears[best]) - metres))
                best = i;
        return best;
    }
    static int SssWeaponIndex(float t)
    {
        int best = 0;
        for (int i = 1; i < 5; i++)
            if (fabsf(kSssWeaponThick[i] - t) < fabsf(kSssWeaponThick[best] - t))
                best = i;
        return best;
    }
    static int SssReachIndex(float metres)
    {
        int best = 0;
        for (int i = 1; i < 6; i++)
            if (fabsf(kSssReaches[i] - metres) < fabsf(kSssReaches[best] - metres))
                best = i;
        return best;
    }
    template <int Bit> static int BakedGet()
    {
        const int v = g_giControl.bakedOff;
        return ((v < 0 ? GiBase().bakedOff : v) & Bit) ? 1 : 0;
    }
    template <int Bit> static int BakedDef() { return (GiBase().bakedOff & Bit) ? 1 : 0; }
    template <int Bit> static void BakedSet(int on)
    {
        const int v = g_giControl.bakedOff, cur = v < 0 ? GiBase().bakedOff : v;
        g_giControl.bakedOff = on ? cur | Bit : cur & ~Bit;
    }
    static const std::vector<PanelItem>& PanelItems()
    {
        static const char* const onOff[] = {"ВЫКЛ|OFF", "ВКЛ|ON"};
        static const char* const ao[] = {"SSAO ИГРЫ|GAME'S SSAO", "GTAO"};
        static const char* const gtaoAcc[] = {"ВЫКЛ|OFF", "0.5", "0.7", "0.9"};
        static const char* const contact[] = {"ВЫКЛ|OFF", "СТАРЫЕ, 12 ШАГОВ|OLD, 12 STEPS", "SSS (BEND)"};
        static const char* const sssReach[] = {"100 М|100 M", "200 М|200 M", "500 М|500 M", "1000 М|1000 M", "2000 М|2000 M", "4000 М|4000 M"};
        static const char* const sssWeapon[] = {"КАК МИР|AS THE WORLD", "+2%", "+5%", "+10%", "+20%"}; // a second window, the darker
        static const char* const outdoor[] = {"50%", "70%", "100%", "150%", "200%"};
        static const char* const baked[] = {"ДВИЖОК|ENGINE", "GI"};
        static const char* const view[] = {"ВЫКЛ|OFF", "КАРКАС|WIRE", "СПЛОШНОЙ|SOLID"};
        static const char* const geometry[] = {"ВЫКЛ|OFF", "ВКЛ|ON", "ВКЛ + ЛИСТВА|ON + FOLIAGE"};
        static const char* const opacity[] = {"ЭКРАН И ГЕОМЕТРИЯ|SCREEN+GEOMETRY", "ГЕОМЕТРИЯ|GEOMETRY"};
        static const char* const cellLight[] = {"ОДИН ЦВЕТ|ONE COLOUR", "ЦВЕТ + НАПРАВЛЕНИЕ|COLOUR + DIRECTION",
                                                "6 НАПРАВЛЕНИЙ|6 DIRECTIONS"};
        static const char* const env[] = {"КАРТЫ ИГРЫ|GAME'S MAPS", "ВОКСЕЛЬНЫЙ КУБ|VOXEL CUBE", "КУБ ДВИЖКА|ENGINE CUBE"};
        static const char* const water[] = {"ПЛАНАРНОЕ|PLANAR", "КУБКАРТА|CUBE MAP", "SSR + КУБКАРТА|SSR + CUBE MAP"};
        static const char* const waterSource[] = {"ВОКСЕЛИ|VOXELS", "ДВИЖОК|ENGINE"};
        static const char* const cubeCull[] = {"КАК В ДВИЖКЕ|ENGINE'S", "НЕТ|NONE", "ОБРАТНОЕ|INVERTED", "ПО ГРАНЯМ|PER FACE"};
        static const char* const cubeMip[] = {"АВТО|AUTO", "0", "1", "2", "3", "4", "5", "6", "7", "8"};
        static const char* const cubeBlend[] = {"ВЫКЛ|OFF", "50%", "25%"};
        static const char* const cubeSize[] = {"64", "128", "256", "512"};
        static const char* const giNear[] = {"30 М (ДО 75)|30 M (TO 75)", "40 М (ДО 100)|40 M (TO 100)", "50 М (ДО 125)|50 M (TO 125)",
                                             "62 М (ДО 155)|62 M (TO 155)"};
        static const char* const giMethod[] = {"КОНУСЫ|CONES", "ПРОБЫ (GI v2)|PROBES (GI v2)", "СВЕТ ПРОБ, ТЕНЬ КОНУСОВ|PROBE LIGHT, CONE SHADE"};
        static const char* const cubePicture[] = {"Num4/6: СНЯТЬ|Num4/6: TAKE", "Num4/6: СНЯТЬ|Num4/6: TAKE"};
        static const char* const cubeRate[] = {"КАЖДЫЙ КАДР|EVERY FRAME", "РАЗ В 2 КАДРА|EVERY 2ND FRAME", "РАЗ В 3 КАДРА|EVERY 3RD FRAME",
                                               "РАЗ В 4 КАДРА|EVERY 4TH FRAME", "РАЗ В 6 КАДРОВ|EVERY 6TH FRAME", "РАЗ В 8 КАДРОВ|EVERY 8TH FRAME"};
        static const int outdoorValues[5] = {50, 70, 100, 150, 200};
        static const char* const fogGi[] = {"50%", "100%", "150%", "200%"};
        static const int fogGiValues[4] = {50, 100, 150, 200};
        static const char* const fogSun[] = {"ВЫКЛ|OFF", "50%", "100%"};
        static const char* const fogHaze[] = {"ВЫКЛ|OFF", "10%", "20%", "35%", "50%"};
        static const int fogHazeValues[5] = {0, 10, 20, 35, 50};
        static const char* const fogSamples[] = {"1", "4", "8"};
        static const char* const dlssModel[] = {"K (ТРАНСФОРМЕР)|K (TRANSFORMER)", "J", "L (DLSS 4.5)|L (DLSS 4.5)", "M (DLSS 4.5)|M (DLSS 4.5)",
                                                "F (CNN)|F (CNN)", "E (CNN)|E (CNN)"};
        static const char* const dlssCompare[] = {"ВЫКЛ|OFF", "СПРАВА K|RIGHT: K", "СПРАВА J|RIGHT: J", "СПРАВА L|RIGHT: L", "СПРАВА M|RIGHT: M",
                                                  "СПРАВА F|RIGHT: F", "СПРАВА E|RIGHT: E"};
        static const std::vector<PanelItem> items = {
            // GI
            {0, "GI|GI", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.enabled ? 1 : 0; }, [] { return 1; },
             [](int v) { g_giControl.enabled = v != 0; }, true, nullptr},
            {0, "Окклюзия под укрытием|Occlusion under cover", 0, 30, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.occlusionScale * 10.0f)); }, [] { return 10; },
             [](int v) { g_giControl.occlusionScale = v / 10.0f; }, true, nullptr},
            {0, "Отражённый свет|Bounce light", 0, 40, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.bounceScale * 10.0f)); }, [] { return 10; },
             [](int v) { g_giControl.bounceScale = v / 10.0f; }, true, nullptr},
            {0, "На оружии и руках|On weapon and hands", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.weapon ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.weapon = v != 0; }, true, nullptr},
            {1, "Затенение|Ambient occlusion", 0, 1, 1, true, ao, 0, nullptr,
             [] { const int v = g_giControl.gtao; return v < 0 ? (GiBase().replaceSsao ? 1 : 0) : v; },
             [] { return GiBase().replaceSsao ? 1 : 0; }, [](int v) { g_giControl.gtao = v; }, true, nullptr},
            // GTAO's own history (the user, 2026-11-14: noisy on small objects and leaves while moving; 0.9 swam): off as
            // before (DLAA accumulates the turning pattern), or mixed with the last frame at 0.5 / 0.7 / 0.9.
            {1, "Накопление GTAO|GTAO accumulation", 0, 3, 1, false, gtaoAcc, 0, nullptr,
             [] { const int v = g_giControl.gtaoHistory; return v >= 0 ? std::min(v, 3) : GtaoHistoryIndex(GiBase()); },
             [] { return GtaoHistoryIndex(GiBase()); }, [](int v) { g_giControl.gtaoHistory = v; }, true, nullptr},
            {1, "Контактные тени|Contact shadows", 0, 2, 1, true, contact, 0, nullptr,
             [] { const int v = g_giControl.contact; return v < 0 ? BaseContact() : v; },
             [] { return BaseContact(); }, [](int v) { g_giControl.contact = v; }, true, nullptr},
            {1, "Дальность SSS|SSS reach", 0, 5, 1, false, sssReach, 0, nullptr,
             [] { const int v = g_giControl.sssReach; return SssReachIndex(v > 0 ? static_cast<float>(v) : GiBase().sssReach); },
             [] { return SssReachIndex(GiBase().sssReach); }, [](int v) { g_giControl.sssReach = kSssReaches[v]; }, true, nullptr},
            {1, "SSS: толщина рук и оружия|SSS: hands and weapon thickness", 0, 4, 1, false, sssWeapon, 0, nullptr,
             [] { const int v = g_giControl.sssWeapon; return v >= 0 ? v : SssWeaponIndex(GiBase().sssWeaponThickness); },
             [] { return SssWeaponIndex(GiBase().sssWeaponThickness); }, [](int v) { g_giControl.sssWeapon = v; }, true, nullptr},
            // How far the GI works in full; it fades out by 2.5 x that and the engine's baked occlusion comes back (the
            // user, Sponza 2026-11-13: "everything far away much lighter" - where the baked occlusion is weak).
            {0, "GI в полную силу до|GI at full strength up to", 0, 3, 1, false, giNear, 0, nullptr,
             [] { const int v = g_giControl.giNear; return v >= 0 ? v : GiNearIndex(GiBase().terrainAoNear); },
             [] { return GiNearIndex(GiBase().terrainAoNear); }, [](int v) { g_giControl.giNear = v; }, true, nullptr},
            // GI v2 (CRYMT_GI_PROBES): probes traced through the voxels near the camera in place of the cones.
            {0, "Метод GI|GI method", 0, 2, 1, true, giMethod, 0, nullptr,
             [] { const int v = g_giControl.probes; return v < 0 ? GiBase().probes : std::min(v, 2); },
             [] { return GiBase().probes; }, [](int v) { g_giControl.probes = v; }, true, nullptr},
            {0, "GI на дыме и пыли|GI on smoke and dust", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.particles; return v < 0 ? (GiBase().particlesGi ? 1 : 0) : v; },
             [] { return GiBase().particlesGi ? 1 : 0; }, [](int v) { g_giControl.particles = v; }, true, nullptr},
            {0, "Дым снаружи темнее|Smoke darker outdoors", 0, 4, 1, true, outdoor, 0, nullptr,
             [] {
                 const int v = g_giControl.particleOut;
                 if (v < 0)
                     return BaseParticleOut();
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [] { return BaseParticleOut(); }, [](int v) { g_giControl.particleOut = outdoorValues[v]; }, true, nullptr},
            // Volumetric fog, stage 1 (RunVfog): the engine's fog darkened where the GI sees shade (rooms, under cover).
            {0, "GI в тумане|GI in the fog", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.vfog; return v < 0 ? std::min(GiBase().vfog, 1) : std::min(v, 1); },
             [] { return std::min(GiBase().vfog, 1); }, [](int v) { g_giControl.vfog = v; }, true, nullptr},
            {0, "Сила GI в тумане|GI strength in the fog", 0, 3, 1, false, fogGi, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogGi >= 0 ? g_giControl.vfogGi.load() : static_cast<int>(GiBase().vfogGi * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 4; i++)
                     if (abs(fogGiValues[i] - v) < abs(fogGiValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogGi * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 4; i++)
                     if (abs(fogGiValues[i] - v) < abs(fogGiValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogGi = fogGiValues[v]; }, true, nullptr},
            // Stage 2: the sun's shadow maps in the fog (its haze dark in the shade outdoors too).
            {0, "Тени солнца в тумане|Sun shadows in the fog", 0, 2, 1, true, fogSun, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogSun >= 0 ? g_giControl.vfogSun.load() : static_cast<int>(GiBase().vfogSun * 100.0f + 0.5f);
                 return v <= 25 ? 0 : v <= 75 ? 1 : 2;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogSun * 100.0f + 0.5f);
                 return v <= 25 ? 0 : v <= 75 ? 1 : 2;
             },
             [](int v) { g_giControl.vfogSun = v * 50; }, true, nullptr},
            // The sun's shadow samples per cell (smoother shadow edges in the fog, a little more time).
            {0, "Выборки теней в тумане|Fog shadow samples", 0, 2, 1, true, fogSamples, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogSamples >= 0 ? g_giControl.vfogSamples.load() : GiBase().vfogSamples;
                 return v >= 8 ? 2 : v >= 4 ? 1 : 0;
             },
             [] { return GiBase().vfogSamples >= 8 ? 2 : GiBase().vfogSamples >= 4 ? 1 : 0; },
             [](int v) { g_giControl.vfogSamples = v == 2 ? 8 : v == 1 ? 4 : 1; }, true, nullptr},
            // The near air: a haze of its own near the camera (the sun's shafts in a thin fog).
            {0, "Плотность воздуха вблизи|Near air density", 0, 4, 1, false, fogHaze, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogHaze >= 0 ? g_giControl.vfogHaze.load() : static_cast<int>(GiBase().vfogHaze * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(fogHazeValues[i] - v) < abs(fogHazeValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogHaze * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(fogHazeValues[i] - v) < abs(fogHazeValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogHaze = fogHazeValues[v]; }, true, nullptr},
            // Its own darkening outdoors (the smoke's row above is the particles' only).
            {0, "Туман снаружи темнее|Fog darker outdoors", 0, 4, 1, true, outdoor, 0, nullptr,
             [] {
                 const int v = g_giControl.vfogOut >= 0 ? g_giControl.vfogOut.load() : static_cast<int>(GiBase().vfogOutdoor * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [] {
                 const int v = static_cast<int>(GiBase().vfogOutdoor * 100.0f + 0.5f);
                 int best = 0;
                 for (int i = 1; i < 5; i++)
                     if (abs(outdoorValues[i] - v) < abs(outdoorValues[best] - v))
                         best = i;
                 return best;
             },
             [](int v) { g_giControl.vfogOut = outdoorValues[v]; }, true, nullptr},
            {1, "Запечённый AO: объекты|Baked AO: objects (RAM)", 0, 1, 1, true, baked, 0, nullptr, BakedGet<2>, BakedDef<2>,
             BakedSet<2>, true, nullptr},
            {1, "Запечённый AO: небо|Baked AO: objects' sky", 0, 1, 1, true, baked, 0, nullptr, BakedGet<4>, BakedDef<4>,
             BakedSet<4>, true, nullptr},
            {1, "Запечённый AO: деревья|Baked AO: under trees", 0, 1, 1, true, baked, 0, nullptr, BakedGet<8>, BakedDef<8>,
             BakedSet<8>, true, nullptr},
            {1, "Запечённый AO: террейн|Baked AO: terrain", 0, 1, 1, true, baked, 0, nullptr, BakedGet<16>, BakedDef<16>,
             BakedSet<16>, true, nullptr},
            // Voxels
            {2, "Вид вокселей|Voxel view", 0, 2, 1, true, view, 0, nullptr, [] { return g_giControl.view.load(); }, [] { return 0; },
             [](int v) { g_giControl.view = v; }, true, nullptr},
            {2, "Геометрия в вокселях|Geometry in voxels", 0, 2, 1, true, geometry, 0, nullptr,
             [] {
                 const int v = g_giControl.geometry;
                 return v >= 0 ? v : GiBase().geometry ? (GiBase().geometryFoliage ? 2 : 1) : 0;
             },
             [] { return GiBase().geometry ? (GiBase().geometryFoliage ? 2 : 1) : 0; }, [](int v) { g_giControl.geometry = v; }, true,
             nullptr},
            {2, "Непрозрачность из|Opacity from", 0, 1, 1, true, opacity, 0, nullptr,
             [] { const int v = g_giControl.voxOpacity; return v < 0 ? (GiBase().opacityGeometry ? 1 : 0) : v; },
             [] { return GiBase().opacityGeometry ? 1 : 0; }, [](int v) { g_giControl.voxOpacity = v; }, true, nullptr},
            // The cells' light (CRYMT_GI_DIRLIGHT): one colour for all directions, or a colour and the direction it goes
            // out stronger (a wall lit outside stays dark inside).
            {2, "Свет ячейки|Cell light", 0, 2, 1, true, cellLight, 0, nullptr,
             [] { const int v = g_giControl.dirLight; return v < 0 ? GiBase().dirLight : std::min(v, 2); },
             [] { return GiBase().dirLight; }, [](int v) { g_giControl.dirLight = v; }, true, nullptr},
            // Reflections
            {3, "Отражения|Reflections", 0, 2, 1, true, env, 0, nullptr,
             [] { const int v = g_giControl.env; return v < 0 ? EnvModeBase() : v; }, [] { return EnvModeBase(); },
             [](int v) { g_giControl.env = v; }, true, nullptr},
            {3, "Отражение воды|Water reflection", 0, 2, 1, true, water, 0, nullptr, [] { return WaterMode(); },
             [] { return WaterModeBase(); },
             [](int v) {
                 g_giControl.waterCube = v;
                 CryMTConsoleCommand(v ? "r_WaterReflections 0" : "r_WaterReflections 1"); // the engine's planar pass
             },
             true, nullptr},
            {3, "Кубкарта воды|Water cube map", 0, 1, 1, true, waterSource, 0, nullptr, [] { return EngineCubeOn() ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.waterSource = v; }, true, nullptr},
            {3, "Грань кубкарты рисуется|Cube map face drawn", 0, 5, 1, false, cubeRate, 0, nullptr,
             [] { const int v = g_giControl.envCubeRate; return EnvCubeRateIndex(v > 0 ? v : EnvCubeRateBase()); },
             [] { return EnvCubeRateIndex(EnvCubeRateBase()); }, [](int v) { g_giControl.envCubeRate = kEnvCubeRates[v]; }, true, nullptr},
            {3, "Отсечение в кубкарте|Cube map culling", 0, 3, 1, true, cubeCull, 0, nullptr, [] { return EngineCubeCull(); },
             [] { return EngineCubeCullBase(); }, [](int v) { g_giControl.envCubeCull = v; }, true, nullptr},
            {3, "Mip кубкарты (отладка)|Cube map mip (debug)", 0, 9, 1, false, cubeMip, 0, nullptr,
             [] { return std::max(g_giControl.envCubeMip.load(), -1) + 1; }, [] { return 0; }, [](int v) { g_giControl.envCubeMip = v - 1; }, true,
             nullptr},
            // r_EnvCMResolution, the renderer's int at 0x382E9848 (0 64, 1 128, 2 256, 3 512): the engine makes its cube anew.
            {3, "Размер кубкарты|Cube map size", 0, 3, 1, false, cubeSize, 0, nullptr,
             [] { return std::min(std::max(*reinterpret_cast<const int*>(0x382E9848), 0), 3); }, [] { return 2; },
             [](int v) {
                 char command[48];
                 snprintf(command, sizeof(command), "r_EnvCMResolution %d", v);
                 CryMTConsoleCommand(command);
             },
             true, nullptr},
            {3, "Смешивание граней|Face blending", 0, 2, 1, true, cubeBlend, 0, nullptr, [] { return EngineCubeBlendIndex(EngineCubeBlend()); },
             [] { return EngineCubeBlendIndex(EngineCubeBlendBase()); }, [](int v) { g_giControl.envCubeBlend = v; }, true, nullptr},
            {3, "Сущности в кубкарте|Entities in the cube", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeEntities; return v < 0 ? EngineCubeEntitiesBase() : v; }, [] { return EngineCubeEntitiesBase(); },
             [](int v) { g_giControl.envCubeEntities = v; }, true, nullptr},
            // The clouds apart from the other entities (the user, 2026-11-14): off by default (cut at the faces' seams,
            // drawn inside rooms).
            {3, "Облака в кубкарте|Clouds in the cube", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeClouds; return v < 0 ? EngineCubeCloudsBase() : v; }, [] { return EngineCubeCloudsBase(); },
             [](int v) { g_giControl.envCubeClouds = v; }, true, nullptr},
            {3, "Нижняя грань кубкарты|Cube map down face", 0, 1, 1, true, onOff, 0, nullptr,
             [] { const int v = g_giControl.envCubeDown; return v < 0 ? EngineCubeDownBase() : v; }, [] { return EngineCubeDownBase(); },
             [](int v) { g_giControl.envCubeDown = v; }, true, nullptr},
            {3, "Снимок кубкарты (logs\\play)|Cube map picture (logs\\play)", 0, 1, 1, true, cubePicture, 0, nullptr, [] { return 0; },
             [] { return 0; }, [](int) { g_cubeDumpWanted = true; }, false, nullptr},
            {3, "Небо в отражении воды|Sky in the water's reflection", 2, 20, 1, false, nullptr, 0.1f, "x%.1f",
             [] { return static_cast<int>(lroundf(WaterSky() * 10.0f)); }, [] { return static_cast<int>(lroundf(WaterSkyBase() * 10.0f)); },
             [](int v) { g_giControl.waterSky = v / 10.0f; }, true, nullptr},
            {3, "На всех материалах|On every material", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.envAll ? 1 : 0; },
             [] { return 1; }, [](int v) { g_giControl.envAll = v != 0; }, true, nullptr},
            {3, "Материалы со своей картой|Materials with a map", 0, 16, 1, false, nullptr, 0.5f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.envOwnScale * 2.0f)); }, [] { return 2; },
             [](int v) { g_giControl.envOwnScale = v / 2.0f; }, true, nullptr},
            {3, "Все материалы|Every material", 0, 16, 1, false, nullptr, 0.5f, "x%.1f",
             [] { return static_cast<int>(lroundf(g_giControl.envAllScale * 2.0f)); }, [] { return 2; },
             [](int v) { g_giControl.envAllScale = v / 2.0f; }, true, nullptr},
            // Engine
            {4, "Кэш дальних теней|Far shadow cache", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.gsmCache > 0 ? 1 : 0; },
             [] { const int d = g_giControl.gsmDefault; return d < 0 ? (g_giControl.gsmCache > 0 ? 1 : 0) : d; },
             [](int v) {
                 CryMTConsoleCommand(v ? "e_gsm_cache 1" : "e_gsm_cache 0"); // through the console: the engine's own handling
                 g_giControl.gsmCache = v;
             },
             true, [](bool ru) { return std::string(g_giControl.gsmCache < 0 ? "?" : g_giControl.gsmCache ? (ru ? "ВКЛ" : "ON") : (ru ? "ВЫКЛ" : "OFF")); }},
            {4, "DLAA в копиях кадра|DLAA into the frame's copies", 0, 1, 1, true, onOff, 0, nullptr,
             [] { return g_giControl.fixCopies ? 1 : 0; },
             [] {
                 wchar_t t[8] = {};
                 return GetEnvironmentVariableW(L"CRYMT_DLSS_FIXCOPIES", t, 8) && wcscmp(t, L"0") == 0 ? 0 : 1;
             },
             [](int v) { g_giControl.fixCopies = v; }, true, nullptr},
            {4, "Модель DLAA|DLAA model", 0, 5, 1, true, dlssModel, 0, nullptr,
             [] { const int v = g_giControl.dlssPreset; return v < 0 ? dlss::PresetDefaultIndex() : v; },
             [] { return dlss::PresetDefaultIndex(); }, [](int v) { g_giControl.dlssPreset = v; }, true, nullptr},
            // A second DLSS with another model on the right half (each half its own history; DLAA's cost twice).
            {4, "Сравнить модели (слева/справа)|Compare models (left/right)", 0, 6, 1, true, dlssCompare, 0, nullptr,
             [] { return g_giControl.dlssCompare + 1; }, [] { return 0; }, [](int v) { g_giControl.dlssCompare = v - 1; }, true, nullptr},
            // Time
            {5, "Время суток|Time of day", 0, 47, 2, true, nullptr, 0, nullptr,
             [] { const float t = g_giControl.timeOfDay; return t < 0 ? 0 : static_cast<int>(lroundf(t * 2.0f)) % 48; },
             [] { return static_cast<int>(lroundf(g_giControl.startTime * 2.0f)) % 48; },
             [](int v) { CryMTSetTimeOfDay(v * 0.5f); }, false,
             [](bool) {
                 const float t = g_giControl.timeOfDay;
                 if (t < 0)
                     return std::string("?");
                 const int minutes = static_cast<int>(t * 60.0f + 0.5f) % (24 * 60);
                 char b[16];
                 snprintf(b, sizeof(b), "%02d:%02d", minutes / 60, minutes % 60);
                 return std::string(b);
             }},
            {5, "Ускоренное время|Fast time", 0, 1, 1, true, onOff, 0, nullptr, [] { return g_giControl.fastTime ? 1 : 0; },
             [] { return 0; }, [](int v) { g_giControl.fastTime = v != 0; }, true, nullptr},
        };
        return items;
    }

    // A key of the panel (main thread, CryMT.cpp's GiHotkeys).
    static void PanelKey(int action)
    {
        GiControl& c = g_giControl;
        if (action == kPanelShow)
        {
            c.panel = !c.panel;
            return;
        }
        if (!c.panel)
            return;
        const int tab = c.tab;
        if (action == kPanelTabPrev || action == kPanelTabNext)
        {
            c.tab = (tab + (action == kPanelTabNext ? 1 : kPanelTabs - 1)) % kPanelTabs;
            return;
        }
        std::vector<const PanelItem*> rows;
        for (const PanelItem& it : PanelItems())
            if (it.tab == tab)
                rows.push_back(&it);
        if (rows.empty())
            return;
        const int n = static_cast<int>(rows.size());
        int row = std::min(std::max(c.rows[tab].load(), 0), n - 1);
        if (action == kPanelUp || action == kPanelDown)
        {
            c.rows[tab] = (row + (action == kPanelDown ? 1 : n - 1)) % n;
            return;
        }
        const PanelItem& it = *rows[row];
        int v = it.get();
        if (action == kPanelDefault)
            v = it.def();
        else
        {
            v += action == kPanelMore ? it.step : -it.step;
            const int range = it.hi - it.lo + 1;
            v = it.wrap ? it.lo + ((v - it.lo) % range + range) % range : std::min(std::max(v, it.lo), it.hi);
        }
        it.set(v);
        Log("panel: %s = %d", PanelLang(it.label, false).c_str(), v);
    }

    static std::wstring Wide(const std::string& s)
    {
        std::wstring w(s.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], static_cast<int>(w.size()));
        w.resize(n > 0 ? n : 0);
        return w;
    }

    // The panel into its texture when it changed. False when there is none.
    bool UpdatePanel(UINT screenW, UINT screenH)
    {
        if (!g_giControl.active || !g_giControl.panel)
            return false;
        const bool ru = PanelRussian();
        const GiSettings& b = GiBase();
        const COLORREF white = RGB(225, 225, 225), dim = RGB(140, 150, 165), changed = RGB(255, 165, 60), title = RGB(250, 210, 90),
                       warn = RGB(240, 90, 80), barColour = RGB(28, 46, 78);
        struct Line
        {
            std::wstring left, right;
            COLORREF lc, rc;
            bool bar; // the selected row's background
        };
        std::vector<Line> lines;
        const int tab = std::min(std::max(g_giControl.tab.load(), 0), kPanelTabs - 1);
        if (g_giControl.numLockOff)
            lines.push_back({Wide(ru ? "NumLock выключен: включите его" : "NumLock is off: turn it on"), L"", warn, warn, false});
        // The tabs: the current one in brackets.
        {
            std::string t = "CryMT ";
            for (int i = 0; i < kPanelTabs; i++)
                t += (i == tab ? "[" : " ") + PanelLang(PanelTabName(i), ru) + (i == tab ? "]" : " ");
            lines.push_back({Wide(t), L"", title, title, false});
        }
        std::vector<const PanelItem*> rows;
        for (const PanelItem& it : PanelItems())
            if (it.tab == tab)
                rows.push_back(&it);
        const int selected = rows.empty() ? -1 : std::min(std::max(g_giControl.rows[tab].load(), 0), static_cast<int>(rows.size()) - 1);
        for (int i = 0; i < static_cast<int>(rows.size()); i++)
        {
            const PanelItem& it = *rows[i];
            const int v = it.get();
            std::string value;
            if (it.text)
                value = it.text(ru);
            else if (it.names)
                value = PanelLang(it.names[std::min(std::max(v, it.lo), it.hi) - it.lo], ru);
            else
            {
                char buf[32];
                snprintf(buf, sizeof(buf), it.format, v * it.scale);
                value = buf;
            }
            const bool off = it.mark && v != it.def();
            lines.push_back({Wide((i == selected ? "> " : "  ") + PanelLang(it.label, ru) + (off ? " *" : "")), Wide(value),
                             i == selected ? white : RGB(200, 200, 200), off ? changed : white, i == selected});
        }
        // Notes of the tab, then the keys.
        std::vector<std::string> notes;
        if (tab == 0 && !b.on)
            notes.push_back(ru ? "GI выключен в CryMT.ini (GI=1)" : "GI is off in CryMT.ini (GI=1)");
        if (tab == 3)
        {
            if (!b.on)
                notes.push_back(ru ? "нужен включённый GI" : "they need the GI on");
            notes.push_back(ru ? "не растительность и не террейн" : "not vegetation, terrain");
        }
        if (tab == 4)
            notes.push_back(ru ? "дальние каскады теней перерисовываются реже" : "far shadow cascades redrawn less often");
        if (tab == 5)
        {
            const int start = static_cast<int>(g_giControl.startTime * 60.0f + 0.5f) % (24 * 60);
            char buf[96];
            snprintf(buf, sizeof(buf), ru ? "Num5 на времени: %02d:%02d" : "Num5 on the time: %02d:%02d", start / 60, start % 60);
            notes.push_back(buf);
        }
        for (const std::string& n : notes)
            lines.push_back({Wide("  " + n), L"", dim, dim, false});
        lines.push_back({Wide(ru ? "Num7/9 вкладка  Num8/2 строка  Num4/6 значение" : "Num7/9 tab  Num8/2 row  Num4/6 value"), L"", dim,
                         dim, false});
        lines.push_back({Wide(ru ? "Num5 сброс  Num+/- время  Num/ скрыть" : "Num5 default  Num+/- time  Num/ hide"), L"", dim, dim, false});
        std::wstring text;
        for (const Line& l : lines)
            text += l.left + L"|" + l.right + (l.rc == changed ? L"*" : L"") + (l.bar ? L"#" : L"") + L"\n";
        text += std::to_wstring(screenW) + L"x" + std::to_wstring(screenH);
        if (text == m_panelText && m_panelTex)
            return true;
        m_panelText = text;
        const int px = static_cast<int>(screenH / 60 > 12 ? screenH / 60 : 12), lineH = px * 5 / 4, pad = px / 2;
        HDC dc = CreateCompatibleDC(nullptr);
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);
        int leftW = 0, rightW = 0, wholeW = 0;
        for (const Line& l : lines)
        {
            SIZE s{};
            GetTextExtentPoint32W(dc, l.left.c_str(), static_cast<int>(l.left.size()), &s);
            if (l.right.empty())
                wholeW = std::max(wholeW, static_cast<int>(s.cx));
            else
            {
                leftW = std::max(leftW, static_cast<int>(s.cx));
                SIZE r{};
                GetTextExtentPoint32W(dc, l.right.c_str(), static_cast<int>(l.right.size()), &r);
                rightW = std::max(rightW, static_cast<int>(r.cx));
            }
        }
        const int gap = px * 2, rightX = pad + leftW + gap;
        const int w = std::max(rightX + rightW, pad + wholeW) + pad, h = static_cast<int>(lines.size()) * lineH + 2 * pad;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits)
        {
            SelectObject(dc, oldFont);
            DeleteObject(font);
            DeleteDC(dc);
            return false;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        memset(bits, 0, static_cast<size_t>(w) * h * 4);
        SetBkMode(dc, TRANSPARENT);
        HBRUSH barBrush = CreateSolidBrush(barColour);
        for (size_t i = 0; i < lines.size(); i++)
        {
            const int y = pad + static_cast<int>(i) * lineH;
            const Line& l = lines[i];
            if (l.bar)
            {
                RECT r{pad / 2, y - (lineH - px) / 2, w - pad / 2, y - (lineH - px) / 2 + lineH};
                FillRect(dc, &r, barBrush);
            }
            SetTextColor(dc, l.lc);
            TextOutW(dc, pad, y, l.left.c_str(), static_cast<int>(l.left.size()));
            if (!l.right.empty())
            {
                SetTextColor(dc, l.rc);
                TextOutW(dc, rightX, y, l.right.c_str(), static_cast<int>(l.right.size()));
            }
        }
        DeleteObject(barBrush);
        GdiFlush();
        // Into a texture (B8G8R8A8, as GDI wrote it).
        if (!m_panelTex || m_panelW != static_cast<UINT>(w) || m_panelH != static_cast<UINT>(h))
        {
            DeferRelease(m_panelTex);
            m_panelTex = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = static_cast<UINT>(w);
            d.Height = static_cast<UINT>(h);
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            if (SUCCEEDED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                                         reinterpret_cast<void**>(&m_panelTex))))
            {
                m_panelW = static_cast<UINT>(w);
                m_panelH = static_cast<UINT>(h);
                m_panelState = D3D12_RESOURCE_STATE_COPY_DEST;
            }
        }
        bool ok = false;
        if (m_panelTex)
        {
            D3D12_RESOURCE_DESC rd = m_panelTex->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT64 total = 0;
            g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            ID3D12Resource* upload = CreateBufferResource(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p = nullptr;
            if (upload && SUCCEEDED(upload->Map(0, nullptr, &p)))
            {
                for (int y = 0; y < h; y++)
                    memcpy(static_cast<uint8_t*>(p) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                           static_cast<const uint8_t*>(bits) + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
                upload->Unmap(0, nullptr);
                if (m_panelState != D3D12_RESOURCE_STATE_COPY_DEST)
                    m_barriers.push_back(Transition(m_panelTex, m_panelState, D3D12_RESOURCE_STATE_COPY_DEST));
                FlushBarriers();
                D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                dst.pResource = m_panelTex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.pResource = upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                NoteCopy("panel", dst, 0, 0, 0, src, nullptr);
                m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                m_barriers.push_back(Transition(m_panelTex, D3D12_RESOURCE_STATE_COPY_DEST, kSrvState));
                m_panelState = kSrvState;
                ok = true;
            }
            DeferRelease(upload);
        }
        SelectObject(dc, oldBmp);
        SelectObject(dc, oldFont);
        DeleteObject(bmp);
        DeleteObject(font);
        DeleteDC(dc);
        if (!ok)
            m_panelText.clear();
        return ok;
    }
