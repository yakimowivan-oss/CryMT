// CryMT launcher - the one place that describes every setting: presets, sections, options, hints, hotkeys.
// The UI (app.js) is generated from this; the native host will read the same data to write CryMT\CryMT.ini.
//
// Option fields:
//   id        unique name
//   section   section id (SECTIONS)
//   label     shown name
//   type      'toggle' | 'slider' | 'select'
//   def       default value
//   min/max/step/unit (slider), choices [{v, label}] (select)
//   ini       [iniSection, key] in CryMT.ini ('CryMT' | 'Env' | 'CVars' | 'Launcher'), or a list of them
//   codec     {write: value -> ini string or null (no line: the code's default), read: ini string or null -> value}
//             the same rules the DLL / proxy use when they read the line (helpers below); for a list of keys
//             write returns and read takes an array
//   hint      what it does (shown in the help panel)
//   affects   what it changes on screen
//   cost      rough GPU cost at 5120x1440 (ms a frame), or a text
//   shaders   true: needs CryMT's shader patch (installed by the launcher)
//   hotkey    the in-game key that toggles it live
//   expert    true: only in the "enthusiast" view
//   needs     another option id that must be on for this one to matter, or 'mtMode' / 'dx12Mode' / 'dlaaMode'
//   offValue  for a select: the value that counts as "off" for options that need it
//   ms        v -> GPU milliseconds a frame for the load meter, 2560x1440 (2026-11-07, preset_bench.ps1): DLAA from
//             the GPU test (MT+DX12 -> MT+DLAA frame time), the GI in all from Village (MT+DLAA -> Full); the GI's
//             parts split by their shares in the GPU timestamps at 5120x1440 (an estimate)
//   personal  true: a preference of the player (window, keys), kept when a preset is picked
window.CRYMT = (function () {
    const t = s => (s == null ? null : String(s).trim());
    const flag = absent => ({write: v => (v ? '1' : '0'), read: s => (t(s) == null ? absent : t(s) !== '0')}); // always written
    const defOn = {write: v => (v ? null : '0'), read: s => t(s) == null || t(s) !== '0'};   // code: on unless "0"
    const defOff = {write: v => (v ? '1' : null), read: s => t(s) != null && t(s) !== '0'};  // code: off unless set, not "0"
    const present = {write: v => (v ? '1' : null), read: s => t(s) != null};                 // code: on when the line exists
    const num = d => ({code: d, write: v => (Math.abs(v - d) < 1e-6 ? null : String(v)),
                       read: s => (t(s) == null || !isFinite(parseFloat(s)) ? d : parseFloat(s))});
    const pick = map => ({write: v => (v in map ? map[v] : null), // value -> ini string; read: undefined = not one of them
                          read: s => Object.keys(map).find(k => map[k] === t(s))});

    // Every text is a pair: L('русский', 'English'). The page shows the half of the launcher's language.
    const L = (ru, en) => ({ru, en});

    const SECTIONS = [
        {id: 'mode', label: L('Режим', 'Mode')},
        {id: 'aa', label: L('Сглаживание', 'Anti-aliasing')},
        {id: 'gi', label: L('Освещение (GI)', 'Lighting (GI)')},
        {id: 'ao', label: L('Затенение и тени', 'Occlusion and shadows')},
        {id: 'particles', label: L('Дым и частицы', 'Smoke and particles')},
        {id: 'baked', label: L('Запечённое затенение', 'Baked occlusion')},
        {id: 'engine', label: L('Движок', 'Engine')},
    ];

    const OPTIONS = [
        // ---- mode
        {id: 'mode', section: 'mode', label: L('Режим работы', 'Mode'), type: 'select', def: 'dlaa',
         choices: [{v: 'off', label: L('Ваниль', 'Vanilla')}, {v: 'mt', label: L('Многопоточность', 'Multithreading')}, {v: 'dx12', label: 'MT + DirectX 12'}, {v: 'dlaa', label: 'MT + DX12 + DLAA'}],
         ini: ['CryMT', 'Mode'], codec: {write: v => v, read: s => (t(s) || 'mt').toLowerCase()},
         hint: L('Многопоточность переносит отрисовку в свой поток: игра упирается в процессор меньше. DirectX 12 заменяет рендер игры на наш. DLAA — сглаживание NVIDIA (нужна видеокарта RTX).', 'Multithreading moves drawing to its own thread, so the game is less CPU bound. DirectX 12 replaces the game\'s renderer with ours. DLAA is NVIDIA\'s anti-aliasing (needs an RTX card).'),
         affects: L('FPS, плавность, сглаживание', 'FPS, smoothness, anti-aliasing'), cost: L('MT: +FPS · DLAA ≈ 1.1 мс', 'MT: +FPS · DLAA ≈ 1.1 ms'), ms: v => (v === 'dlaa' ? 1.1 : 0)},
        {id: 'borderless', section: 'mode', label: L('Окно без рамки', 'Borderless window'), type: 'toggle', def: true, ini: ['CryMT', 'Borderless'], codec: flag(true), personal: true,
         hint: L('Полноэкранное окно без рамки: быстрое переключение Alt+Tab, без смены видеорежима.', 'Fullscreen as a borderless window: instant Alt+Tab, no display mode change.'), affects: L('окно игры', 'game window')},
        {id: 'pause', section: 'mode', label: L('Пауза при сворачивании', 'Pause on focus loss'), type: 'toggle', def: true, ini: ['CryMT', 'PauseOnFocusLoss'], codec: flag(true), personal: true,
         hint: L('Игра встаёт на паузу, когда окно не в фокусе.', 'The game pauses when its window loses focus.'), affects: L('поведение окна', 'window behaviour')},
        {id: 'keys', section: 'mode', label: L('Клавиши CryMT в игре', 'CryMT keys in game'), type: 'toggle', def: true, needs: 'dx12Mode',
         ini: ['Env', 'CRYMT_GI_KEYS'], codec: defOff, personal: true,
         hint: L('Num-блок (с NumLock) и несколько клавиш основной клавиатуры переключают функции на лету — сравнить «было / стало». Список — во вкладке «Клавиши». Изменения действуют до выхода из игры.', 'The numpad (with NumLock) and a few main keyboard keys switch features on the fly to compare before / after. The list is on the Keys tab. Changes last until the game exits.'),
         affects: L('управление в игре', 'in-game controls')},
        {id: 'overlay', section: 'mode', label: L('Оверлей производительности', 'Performance overlay'), type: 'toggle', def: false, personal: true, needs: 'mtMode', hotkey: 'Ctrl+F11',
         ini: ['Env', 'CRYMT_OVERLAY'], codec: present,
         hint: L('Время кадра, загрузка потоков и видеокарты поверх игры. Включается и выключается в игре клавишами Ctrl+F11.', 'Frame time, thread and GPU load over the game. Ctrl+F11 turns it on and off in game.'), affects: L('оверлей поверх игры', 'overlay over the game')},
        {id: 'mod', section: 'mode', label: L('Модификация', 'Mod'), type: 'select', def: '', personal: true, dropdown: true,
         choices: [{v: '', label: L('Без мода', 'No mod')}], // + the folders of <game>\Mods (STATUS.mods)
         ini: ['Launcher', 'Mod'], codec: {write: v => v || null, read: s => t(s) || ''},
         hint: L('Папка из Mods: «Играть» запускает игру с -mod <имя>. CryMT работает и с модами со своей DLL игры. Если мод меняет шейдеры, которые правит GI, GI для него выключается.',
                 'A folder of Mods: Play starts the game with -mod <name>. CryMT works with mods that bring their own game DLL too. If the mod changes the shaders the GI patches, the GI is off for it.'),
         affects: L('какой мод загружается', 'which mod loads')},
        {id: 'modShaders', section: 'mode', label: L('GI с модом, у которого свои шейдеры GI', 'GI with a mod that has its own GI shaders'), type: 'toggle', def: false, personal: true,
         ini: ['Launcher', 'ModShaders'], codec: defOff,
         hint: L('Если мод приносит свои версии шейдеров, которые правит GI, лаунчер выключает для него GI. Включите, чтобы запускать мод с GI на свой риск: игра берёт шейдеры мода, поэтому GI будет виден только там, где мод их не заменил, а смесь шейдеров мода и CryMT может дать ошибки картинки (например, пропавшую землю). Если так — выключите.',
                 'When a mod brings its own versions of the shaders the GI patches, the launcher turns the GI off for it. Turn this on to start the mod with the GI at your own risk: the game uses the mod\'s shaders, so the GI shows only where the mod did not replace them, and a mix of the mod\'s and CryMT\'s shaders can break the picture (for example, missing ground). If so, turn it off.'),
         affects: L('GI с таким модом', 'the GI with such a mod')},
        {id: 'editorCryMT', section: 'mode', label: L('Редактор с CryMT', 'Editor with CryMT'), type: 'toggle', def: true, personal: true, needs: 'mtMode',
         ini: ['Launcher', 'EditorCryMT'], codec: defOn,
         hint: L('Кнопка «Редактор» запускает Sandbox Editor с CryMT в выбранном режиме: DirectX 12, исправления вылетов (в том числе физики). Пауза при потере фокуса и окно без рамки в редакторе не включаются. Пробная поддержка: если редактор ведёт себя странно, выключите. Обычный запуск редактора (не из лаунчера) остаётся без CryMT.',
                 'The Editor button starts the Sandbox Editor with CryMT in the chosen mode: DirectX 12, crash fixes (physics too). The pause on focus loss and the borderless window stay off in the editor. Trial support: if the editor misbehaves, turn it off. A start of the editor outside the launcher stays without CryMT.'),
         affects: L('кнопка «Редактор»', 'the Editor button')},
        {id: 'afterStart', section: 'mode', label: L('После запуска игры', 'After the game starts'), type: 'select', def: 'minimize', personal: true,
         choices: [{v: 'minimize', label: L('Свернуть', 'Minimize')}, {v: 'close', label: L('Закрыть', 'Close')}, {v: 'stay', label: L('Оставить', 'Stay')}],
         ini: ['Launcher', 'AfterStart'], codec: pick({minimize: null, close: 'close', stay: 'stay'}),
         hint: L('Что делает лаунчер, когда игра запущена.', 'What the launcher does once the game is running.'), affects: L('окно лаунчера', 'launcher window')},

        // ---- anti-aliasing
        {id: 'fixCopies', section: 'aa', label: L('Стабилизация DLAA на месте', 'DLAA stable on a still camera'), type: 'toggle', def: true, ini: ['Env', 'CRYMT_DLSS_FIXCOPIES'], codec: defOn, hotkey: L('Num/ ▸ Движок', 'Num/ ▸ Engine'),
         hint: L('Пост-обработка игры читает копии сцены, сделанные до DLAA (с джиттером камеры). Без этого картинка на стоящей камере дрожала по листве и мелким деталям.', 'The game\'s post-processing reads copies of the scene made before DLAA (with the camera jitter). Without this, foliage and fine detail shimmered on a still camera.'),
         affects: L('дрожание на стоящей камере', 'shimmer on a still camera'), cost: L('≈ 0.03 мс', '≈ 0.03 ms'), needs: 'dlaaMode', ms: v => (v ? 0.03 : 0)},
        {id: 'preset', section: 'aa', label: L('Модель DLAA', 'DLAA model'), type: 'select', def: 'auto', expert: true, needs: 'dlaaMode',
         // desc: what each model does besides its cost (NVIDIA's notes in the SDK's sl_dlss.h, CryMT's measurements of 2026-11-13
         // at 2560x1440: the DLAA pass K 1.1 ms, M 3.9 ms, L 4.9 ms; sharpness of the output against the input 0.76 / 0.71 / 0.68).
         choices: [{v: 'auto', label: L('Авто (K)', 'Auto (K)'), desc: L('Что выбирает DLSS для DLAA: сейчас это K.', 'What DLSS picks for DLAA: K now.')},
                   {v: 'K', label: 'K', desc: L('Трансформер DLSS 4, у NVIDIA основной для DLAA. По NVIDIA: лучшее качество среди моделей для DLAA. Наши замеры: самая чёткая из K/L/M, искр в тени меньше, чем у E и L; проход DLAA ≈ 1,1 мс при 2560×1440.',
                                                   'The DLSS 4 transformer, NVIDIA\'s default for DLAA. Per NVIDIA: the best quality of the models for DLAA. Our measurements: the sharpest of K/L/M, fewer sparkles in shade than E and L; the DLAA pass ≈ 1.1 ms at 2560×1440.')},
                   {v: 'J', label: 'J', desc: L('Почти как K. По NVIDIA: чуть меньше шлейфов за движущимся, но больше мерцания; NVIDIA советует K. Цена как у K.',
                                                   'Nearly K. Per NVIDIA: slightly less ghosting behind moving things, more flicker; NVIDIA recommends K. K\'s cost.')},
                   {v: 'L', label: 'L', desc: L('Трансформер второго поколения (DLSS 4.5), сделан для режима Ultra Performance. По NVIDIA: стабильнее и меньше шлейфов, чем J/K. В DLAA у нас картинка заметно мягче K (сглаживает сильнее, мелкие детали тоже), искр в тени почти вдвое больше, чем у K; проход ≈ 4,9 мс — вчетверо дороже K.',
                                                   'The second-generation transformer (DLSS 4.5), made for Ultra Performance. Per NVIDIA: steadier, less ghosting than J/K. In DLAA here the picture is clearly softer than K (smooths more, fine detail too), nearly twice K\'s sparkles in shade; the pass ≈ 4.9 ms, four times K.')},
                   {v: 'M', label: 'M', desc: L('Трансформер DLSS 4.5, сделан для режима Performance. По NVIDIA: качество как у L, скорость ближе к J/K. В DLAA у нас мягче K, чуть резче L; искры в тени не мерились; проход ≈ 3,9 мс.',
                                                   'The DLSS 4.5 transformer made for Performance. Per NVIDIA: L\'s quality at closer to J/K\'s speed. In DLAA here softer than K, a little sharper than L; sparkles in shade not measured; the pass ≈ 3.9 ms.')},
                   {v: 'F', label: 'F', desc: L('Старая свёрточная (CNN) модель DLSS 3, раньше основная для DLAA. Дешевле трансформера; NVIDIA пометила её устаревшей. У нас не измерялась.',
                                                   'The old convolutional (CNN) model of DLSS 3, DLAA\'s default back then. Cheaper than the transformer; NVIDIA marks it deprecated. Not measured here.')},
                   {v: 'E', label: 'E', desc: L('Старая CNN-модель DLSS 3.7 (для режимов Качество/Производительность). В нашем замере искр в тени в 2,2 раза больше, чем у K. NVIDIA пометила её устаревшей.',
                                                   'The old CNN model of DLSS 3.7 (made for Quality/Performance). In our test 2.2 times K\'s sparkles in shade. NVIDIA marks it deprecated.')}],
         ini: ['Env', 'CRYMT_DLSS_PRESET'], codec: pick({auto: null, K: 'K', J: 'J', L: 'L', M: 'M', F: 'F', E: 'E'}),
         hint: L('Нейросеть DLAA. Трансформер (K) лучше держит листву и мелкие детали; L и M — трансформер DLSS 4.5 (по NVIDIA: резче, стабильнее, меньше шлейфов, но дороже); старые CNN (E/F) мягче и дешевле. В игре: Num/ ▸ Движок ▸ «Модель DLAA» (переключение) и «Сравнить модели» (слева выбранная, справа другая — у каждой половины своя история; DLAA считается дважды).', 'The DLAA network. The transformer (K) holds foliage and fine detail better; L and M are the DLSS 4.5 transformer (per NVIDIA: sharper, steadier, less ghosting, dearer); the older CNN ones (E/F) are softer and cheaper. In game: Num/ ▸ Engine ▸ "DLAA model" (switch) and "Compare models" (left the chosen one, right another, each half with its own history; DLAA runs twice).'), affects: L('резкость, шлейфы', 'sharpness, trails')},

        // ---- GI
        {id: 'gi', section: 'gi', label: L('Глобальное освещение', 'Global illumination'), type: 'toggle', def: true, ini: ['CryMT', 'GI'], codec: flag(false), shaders: true, hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'), needs: 'dx12Mode',
         hint: L('Отражённый свет и затенение от окружения: стены и потолки затеняют комнаты, освещённые солнцем поверхности подсвечивают соседние, кроны затемняют землю. Работает в режиме DirectX 12.', 'Bounced light and occlusion from the surroundings: walls and ceilings shade rooms, sunlit surfaces light their neighbours, tree crowns darken the ground. Works in DirectX 12 mode.'),
         affects: L('освещение помещений, углов, под деревьями', 'lighting of rooms, corners, under trees'), cost: L('≈ 0.6 мс основа, всё вместе ≈ 1.5', '≈ 0.6 ms base, ≈ 1.5 in all'), ms: v => (v ? 0.6 : 0)},
        {id: 'giScreen', section: 'gi', label: L('Экранная часть GI', 'Screen part of the GI'), type: 'toggle', def: false, needs: 'gi', expert: true, hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'),
         ini: ['Env', 'CRYMT_GI_SCREEN'], codec: defOff, cost: L('≈ 0.5 мс', '≈ 0.5 ms'), ms: v => (v ? 0.5 : 0),
         hint: L('Отражённый свет и затенение, найденные по экрану, поверх вокселей. По умолчанию выключена: без неё картинка спокойнее, свет и затенение дают воксели, складки — GTAO.', 'Bounced light and occlusion found on screen, on top of the voxels. Off by default: the picture is calmer without it; the voxels give the light and occlusion, GTAO the creases.'),
         affects: L('подсветка у поверхностей, мерцание', 'glow near surfaces, flicker')},
        {id: 'giRadius', section: 'gi', label: L('Радиус экранной части', 'Screen part radius'), type: 'slider', def: 0.75, min: 0.25, max: 3, step: 0.05, unit: L('м', 'm'), needs: 'giScreen', expert: true,
         ini: ['Env', 'CRYMT_GI_RADIUS'], codec: num(2.5),
         hint: L('Как далеко по экрану ищется отражённый свет у каждой точки. Больше — мягче и шире, но больше шума.', 'How far across the screen bounced light is looked for at each point. Larger is softer and wider, but noisier.'), affects: L('размер подсветки у поверхностей', 'size of the glow near surfaces')},
        {id: 'giBounce', section: 'gi', label: L('Сила отскока (экран)', 'Bounce strength (screen)'), type: 'slider', def: 6.7, min: 0, max: 10, step: 0.1, needs: 'giScreen', expert: true, hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'),
         ini: ['Env', 'CRYMT_GI_BOUNCE'], codec: num(1.0),
         hint: L('Насколько ярко освещённые поверхности на экране подсвечивают соседние. Слишком большое значение высветляет углы комнат.', 'How brightly lit surfaces on screen light their neighbours. Too much brightens the corners of rooms.'), affects: L('цветной отражённый свет', 'coloured bounced light')},
        {id: 'giAo', section: 'gi', label: L('Затенение (экран)', 'Occlusion (screen)'), type: 'slider', def: 4, min: 0, max: 8, step: 0.1, needs: 'giScreen', expert: true,
         ini: ['Env', 'CRYMT_GI_AO'], codec: num(0.8),
         hint: L('Сила экранной части затенения окружающего света (поверх вокселей).', 'Strength of the screen part of the ambient occlusion (on top of the voxels).'), affects: L('тёмность углов и под предметами', 'darkness of corners and under objects')},
        {id: 'voxels', section: 'gi', label: L('Воксели вокруг камеры', 'Voxels around the camera'), type: 'toggle', def: true, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_VOXELS'], codec: defOn, cost: L('≈ 1 мс', '≈ 1 ms'),
         hint: L('Сетки 0.25–2 м до 256 м вокруг камеры: свет и затенение от того, чего нет на экране (крыши, стены за спиной). Без них GI — только экранный.', 'Grids of 0.25–2 m cells up to 256 m around the camera: light and occlusion from what is not on screen (roofs, walls behind you). Without them the GI is screen-only.'), affects: L('помещения, крыши, кроны', 'rooms, roofs, tree crowns')},
        {id: 'voxStrength', section: 'gi', label: L('Сила отскока (воксели)', 'Bounce strength (voxels)'), type: 'slider', def: 3, min: 0, max: 5, step: 0.1, needs: 'gi',
         ini: ['Env', 'CRYMT_GI_VOXELSTRENGTH'], codec: num(1.5),
         hint: L('Отражённый свет от геометрии вокруг, в том числе вне экрана (солнце и лампы на стенах, полу, крышах).', 'Light bounced from the geometry around, off screen too (sun and lamps on walls, floors, roofs).'), affects: L('общая подсветка сцены', 'overall fill light')},
        {id: 'voxAo', section: 'gi', label: L('Затенение от вокселей', 'Voxel occlusion'), type: 'slider', def: 1.2, min: 0, max: 2, step: 0.05, needs: 'gi',
         ini: ['Env', 'CRYMT_GI_VOXELAO'], codec: num(0.6),
         hint: L('Насколько окружение (стены, потолок, кроны) закрывает небо. Главный источник затемнения помещений.', 'How much the surroundings (walls, ceiling, crowns) hide the sky. The main source of darkness indoors.'), affects: L('тёмность помещений и под деревьями', 'darkness indoors and under trees')},
        {id: 'giOutdoor', section: 'gi', label: L('Затенение на улице', 'Occlusion outdoors'), type: 'slider', def: 1.0, min: 0, max: 2, step: 0.05, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_OUTDOOR'], codec: num(1.0),
         hint: L('Сила затенения вне помещений (VisArea). В помещениях — своя сила (Num4/Num7 в игре).', 'Occlusion strength outside VisAreas. Indoors has its own (Num4/Num7 in game).'), affects: L('улица', 'outdoors')},
        {id: 'giTemporal', section: 'gi', label: L('Накопление по кадрам', 'Frame accumulation'), type: 'slider', def: 0.7, min: 0, max: 0.95, step: 0.05, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_TEMPORAL'], codec: num(0.75),
         hint: L('Смешивание GI с прошлыми кадрами. Выше — спокойнее картинка, но дольше «догоняет» при движении.', 'Blending of the GI with past frames. Higher is calmer, but slower to catch up when moving.'), affects: L('шум и шлейфы GI', 'GI noise and trails')},
        {id: 'geometry', section: 'gi', label: L('Геометрия в вокселях', 'Geometry in voxels'), type: 'select', def: 'on', needs: 'gi', hotkey: L('Num/ ▸ Воксели', 'Num/ ▸ Voxels'),
         choices: [{v: 'off', label: L('Только видимое', 'Visible only')}, {v: 'on', label: L('Вся геометрия', 'All geometry')}, {v: 'foliage', label: L('Вся + листва целиком', 'All + whole foliage')}],
         ini: ['Env', 'CRYMT_GI_GEOMETRY'], codec: pick({off: '0', on: null, foliage: '2'}), cost: L('≈ 0.7 мс', '≈ 0.7 ms'), ms: v => (v === 'off' ? 0 : 0.7),
         hint: L('Без неё воксели знают только то, что видит камера. С ней — всю геометрию сцены: кроны над головой и крыши затеняют точнее, но полы в помещениях темнее и при входе через портал освещение пересчитывается.', 'Without it the voxels know only what the camera sees. With it they hold all scene geometry: crowns overhead and roofs shade more exactly, but indoor floors get darker and the lighting is recomputed when you enter through a portal.'),
         affects: L('точность затенения, порталы', 'occlusion accuracy, portals')},
        {id: 'voxOpacity', section: 'gi', label: L('Непрозрачность вокселей', 'Voxel opacity from'), type: 'select', def: 'geometry', needs: 'geometry', expert: true, hotkey: L('Num/ ▸ Воксели', 'Num/ ▸ Voxels'),
         choices: [{v: 'geometry', label: L('Из геометрии', 'Geometry')}, {v: 'screen', label: L('Экран + геометрия', 'Screen + geometry')}],
         ini: ['Env', 'CRYMT_GI_OCCUPANCY'], codec: pick({geometry: null, screen: 'screen'}),
         hint: L('Откуда воксели знают, что в ячейке что-то есть. Из геометрии — стабильно: на стоящей камере ячейки меняются в 12 раз реже. Экран + геометрия — как было до 10.11: экран дорисовывал и стирал ячейки при движении.', 'Where the voxels learn that a cell holds something. Geometry is stable: on a still camera cells change 12 times less. Screen + geometry is the way before 10.11: the screen added and erased cells as you moved.'),
         affects: L('стабильность затенения', 'occlusion stability')},
        {id: 'geomCache', section: 'gi', label: L('Неизменные объекты реже', 'Unchanged objects less often'), type: 'toggle', def: false, needs: 'geometry', expert: true, hotkey: L('Num/ ▸ Воксели', 'Num/ ▸ Voxels'),
         ini: ['Env', 'CRYMT_GI_GEOMCACHE'], codec: defOff,
         hint: L('Объект, который не сдвинулся и не анимирован, вокселизируется раз в 4 кадра (его отпечаток в вокселях хранится). Сдвинутые, новые, со скелетом — каждый кадр. Пока выключено: рядом с листвой и движущимися объектами затенение мерцает.', 'An object that has not moved and is not animated is voxelized every 4th frame (its stamp in the voxels is kept). Moved, new and skinned ones every frame. Off for now: next to foliage and moving objects the shading flickers.'),
         affects: L('стоимость вокселизации', 'voxelization cost')},
        {id: 'geomKeep', section: 'gi', label: L('Хранить вокселизированную геометрию', 'Keep voxelized geometry'), type: 'toggle', def: true, needs: 'geometry', expert: true,
         ini: ['Env', 'CRYMT_GI_GEOMPERSIST'], codec: defOn,
         hint: L('Геометрия остаётся в вокселях, пока экран не покажет, что место пусто. Без этого она пропадает через 3 кадра после того, как движок перестал её рисовать.', 'Geometry stays in the voxels until the screen shows the place is empty. Without this it disappears 3 frames after the engine stopped drawing it.'), affects: L('стабильность GI', 'GI stability')},
        {id: 'foliageGi', section: 'gi', label: L('GI на листве', 'GI on leaves'), type: 'slider', def: 1.5, min: 1, max: 2.5, step: 0.1, unit: '×', needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_FOLIAGEGI'], codec: num(1.5),
         hint: L('Усиление затенения и отскока на листве кустов и деревьев.', 'Stronger occlusion and bounce on the leaves of bushes and trees.'), affects: L('кусты, кроны', 'bushes, crowns')},
        {id: 'grassGi', section: 'gi', label: L('GI на траве', 'GI on grass'), type: 'slider', def: 1, min: 0.5, max: 2.5, step: 0.1, unit: '×', needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_GRASSGI'], codec: num(1.0),
         hint: L('Затенение и отскок на траве. Трава сохраняет SSAO движка вместо GTAO: так она выглядит как в оригинале, а не плоской и пересвеченной.', 'Occlusion and bounce on grass. Grass keeps the engine\'s SSAO instead of GTAO: it looks as in the original, not flat and too bright.'), affects: L('трава', 'grass')},
        {id: 'screenClassic', section: 'gi', label: L('Старый метод экранного GI', 'Old screen GI method'), type: 'toggle', def: false, needs: 'giScreen', expert: true,
         ini: ['Env', 'CRYMT_GI_SCREENCLASSIC'], codec: defOff,
         hint: L('Горизонтный метод вместо visibility bitmask. Для сравнения: старый сильнее подсвечивал углы комнат.', 'The horizon method instead of the visibility bitmask. For comparison: the old one lit room corners more.'), affects: L('углы помещений', 'room corners')},
        {id: 'weaponGi', section: 'gi', label: L('GI на оружии и руках', 'GI on weapon and hands'), type: 'toggle', def: true, needs: 'gi', hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'), expert: true,
         ini: ['Env', 'CRYMT_GI_WEAPON'], codec: defOn,
         hint: L('Оружие, руки и предметы в руках затеняются и подсвечиваются окружением, как остальная сцена.', 'The weapon, hands and held objects are shaded and lit by the surroundings like the rest of the scene.'), affects: L('вид от первого лица', 'first-person view')},

        // ---- AO and shadows
        {id: 'gtao', section: 'ao', label: L('Затенение в складках', 'Crease occlusion'), type: 'select', def: 'gtao', offValue: 'ssao', needs: 'gi', hotkey: L('Num/ ▸ AO', 'Num/ ▸ AO'),
         choices: [{v: 'gtao', label: L('GTAO (наше)', 'GTAO (ours)')}, {v: 'ssao', label: L('SSAO движка', 'Engine SSAO')}],
         ini: ['Env', 'CRYMT_GI_SSAO'], codec: pick({gtao: null, ssao: '1'}), cost: L('GTAO ≈ 0.5 мс', 'GTAO ≈ 0.5 ms'), ms: v => (v === 'gtao' ? 0.5 : 0),
         hint: L('GTAO точнее и темнее в щелях и углах; SSAO движка мягче и привычнее (так задумывали художники).', 'GTAO is more exact and darker in gaps and corners; the engine\'s SSAO is softer and more familiar (as the artists intended).'), affects: L('складки, углы, контакт предметов', 'creases, corners, object contact')},
        {id: 'gtaoStrength', section: 'ao', label: L('Сила GTAO', 'GTAO strength'), type: 'slider', def: 1.5, min: 0.5, max: 3, step: 0.1, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAO'], codec: num(1.5), hint: L('Насколько темнеют складки.', 'How dark creases get.'), affects: L('тёмность складок', 'darkness of creases')},
        {id: 'gtaoRadius', section: 'ao', label: L('Радиус GTAO', 'GTAO radius'), type: 'slider', def: 0.5, min: 0.2, max: 2, step: 0.05, unit: L('м', 'm'), needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAORADIUS'], codec: num(0.5), hint: L('Размер затенения вокруг складок.', 'Size of the occlusion around creases.'), affects: L('ширина тени в углах', 'width of the shade in corners')},
        {id: 'gtaoFull', section: 'ao', label: L('GTAO в полном разрешении', 'GTAO at full resolution'), type: 'toggle', def: false, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFULL'], codec: defOff, cost: L('≈ +1.6 мс', '≈ +1.6 ms'), ms: v => (v ? 1.6 : 0),
         hint: L('Резче, но заметно дороже. По умолчанию — половинное разрешение.', 'Sharper, but much more expensive. Half resolution by default.'), affects: L('резкость затенения', 'occlusion sharpness')},
        {id: 'gtaoHistory', section: 'ao', label: L('Накопление GTAO по кадрам', 'GTAO frame accumulation'), type: 'toggle', def: false, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOHISTORY'], codec: defOff,
         hint: L('Меньше шума, но при движении затенение «плывёт» и мылится. По умолчанию выключено: фиксированный узор без истории.', 'Less noise, but the occlusion swims and blurs when moving. Off by default: a fixed pattern without history.'), affects: L('шум и шлейфы', 'noise and trails')},
        {id: 'gtaoFoliage', section: 'ao', label: L('GTAO на растениях', 'GTAO on plants'), type: 'toggle', def: true, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFOLIAGE'], codec: defOn,
         hint: L('Трава и листва тоже получают GTAO. Без него растения плоские (проверено — хуже).', 'Grass and leaves get GTAO too. Without it plants look flat (tested: worse).'), affects: L('трава, кусты', 'grass, bushes')},
        {id: 'gtaoFoliageShade', section: 'ao', label: L('Мягче на растениях в тени', 'Softer on plants in shade'), type: 'toggle', def: true, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFOLIAGESHADE'], codec: defOn,
         hint: L('На траве и листве в тени солнца GTAO работает на 20% силы: там окружающий свет — весь их свет, и затенение выглядело слишком сильным.', 'On grass and leaves in the sun\'s shadow GTAO works at 20%: there the ambient is all their light, and the occlusion looked too strong.'), affects: L('растительность в тени', 'vegetation in shade')},
        {id: 'contact', section: 'ao', label: L('Контактные тени', 'Contact shadows'), type: 'toggle', def: true, needs: 'gi', hotkey: L('Num/ ▸ AO', 'Num/ ▸ AO'), shaders: true,
         ini: ['Env', 'CRYMT_CONTACTSHADOWS'], codec: defOn, cost: L('≈ 0.2 мс', '≈ 0.2 ms'), ms: v => (v ? 0.2 : 0),
         hint: L('Тени от солнца в экранном пространстве (Bend Studio SSS): у основания предметов и от мелких деталей, где карта теней слишком грубая, а также от дальних деревьев и скал за пределами карт теней; оружие отбрасывает тень на руки. Режим (старые / SSS) и дальность — на панели в игре.', 'Screen-space sun shadows (Bend Studio SSS): at the base of objects and from small details where the shadow map is too coarse, and from far trees and rocks beyond the shadow maps; the weapon shadows the hands. Mode (old / SSS) and reach on the in-game panel.'),
         affects: L('объём предметов на солнце, дальний пейзаж', 'depth of objects in the sun, the far landscape')},
        {id: 'contactLength', section: 'ao', label: L('Длина контактных теней', 'Contact shadow length'), type: 'slider', def: 0.35, min: 0.1, max: 1, step: 0.05, unit: L('м', 'm'), needs: 'contact', expert: true,
         ini: ['Env', 'CRYMT_CONTACTLENGTH'], codec: num(0.35), hint: L('Только для старых контактных теней (панель: «Контактные тени» → «Старые»). Длина на 10 м от камеры (дальше растёт).', 'Only for the old contact shadows (panel: "Contact shadows" → "Old"). Length at 10 m from the camera (grows farther away).'), affects: L('длина теней', 'shadow length')},
        {id: 'contactFoliage', section: 'ao', label: L('Растения тоже отбрасывают', 'Plants cast them too'), type: 'toggle', def: true, needs: 'contact', expert: true,
         ini: ['Env', 'CRYMT_CONTACTFOLIAGE'], codec: defOn,
         hint: L('Старое поведение: трава и листья отбрасывают контактные тени (луга темнеют при низком солнце).', 'The old behaviour: grass and leaves cast contact shadows (meadows darken at low sun).'), affects: L('трава, листва', 'grass, leaves')},

        // ---- particles
        {id: 'particles', section: 'particles', label: L('GI для дыма и пыли', 'GI for smoke and dust'), type: 'toggle', def: true, needs: 'gi', hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'), shaders: true,
         ini: ['Env', 'CRYMT_GI_PARTICLES'], codec: defOn, cost: L('≈ 0.15 мс', '≈ 0.15 ms'), ms: v => (v ? 0.15 : 0),
         hint: L('Дым, пыль и туман темнеют в помещениях и под кронами, берут оттенок окружения; солнце в комнатах — по вокселям (без мигания). Огонь, искры и вспышки не трогаются.', 'Smoke, dust and fog darken indoors and under crowns and take the hue of their surroundings; indoors the sun comes from the voxels (no flicker). Fire, sparks and flashes are left alone.'),
         affects: L('дым, пыль, туман', 'smoke, dust, fog')},
        {id: 'particleOut', section: 'particles', label: L('Затемнение на улице', 'Darkening outdoors'), type: 'slider', def: 2.0, min: 0.5, max: 2, step: 0.25, unit: '×', needs: 'particles', hotkey: L('Num/ ▸ GI', 'Num/ ▸ GI'),
         ini: ['Env', 'CRYMT_GI_PARTICLEOUT'], codec: num(2.0), hint: L('Относительно помещений. Лёгкая дымка затемняется меньше, чтобы не пропадала.', 'Relative to indoors. Light haze darkens less so it does not vanish.'), affects: L('дым на улице', 'smoke outdoors')},
        {id: 'particleMax', section: 'particles', label: L('Предел затемнения', 'Darkening limit'), type: 'slider', def: 0.85, min: 0.4, max: 0.95, step: 0.05, needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLEMAX'], codec: num(0.85), hint: L('Какую долю окружающего света частица может потерять максимум.', 'The largest share of ambient light a particle can lose.'), affects: L('самые тёмные места', 'the darkest places')},
        {id: 'particleTint', section: 'particles', label: L('Оттенок окружения', 'Surrounding tint'), type: 'slider', def: 0.5, min: 0, max: 1, step: 0.05, needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLETINT'], codec: num(0.5), hint: L('Насколько дым окрашивается в цвет света вокруг (зелень под кронами, тёплый свет у стен). Яркость не меняется.', 'How much smoke takes the colour of the light around it (green under crowns, warm light near walls). Brightness does not change.'), affects: L('цвет дыма', 'smoke colour')},
        {id: 'particleFine', section: 'particles', label: L('Точность у стен', 'Accuracy near walls'), type: 'slider', def: 0.5, min: 0.25, max: 1, step: 0.05, unit: L('м', 'm'), needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLEFINE'], codec: num(0.5), hint: L('Ячейка мелкого объёма (±32 ячейки вокруг камеры). Меньше — точнее у стен и потолков, но меньше охват.', 'Cell size of the fine volume (±32 cells around the camera). Smaller is more exact near walls and ceilings but covers less.'), affects: L('пыль у стен', 'dust near walls')},

        // ---- baked
        {id: 'bakedFade', section: 'baked', label: L('Вблизи наш GI, дальше запечённое', 'Our GI near, baked further'), type: 'toggle', def: true, needs: 'gi', hotkey: L('Num/ ▸ AO', 'Num/ ▸ AO'), expert: true,
         ini: ['Env', 'CRYMT_GI_BAKED'],
         codec: {write: v => (v ? null : 'engine'), read: s => t(s) == null || t(s).toLowerCase() !== 'engine'},
         hint: L('Движок затеняет по запечённым картам (под деревьями, в складках объектов, землю). Вблизи их заменяет наш GI, дальше они плавно возвращаются, вдали — полностью. Выкл: запечённое везде, поверх GI (вблизи затенение двойное).',
                 'The engine shades from baked maps (under trees, in objects\' creases, the ground). Near the camera our GI replaces them, further they come back smoothly, far away fully. Off: baked everywhere, on top of the GI (double near).'),
         affects: L('затенение вблизи и вдали', 'occlusion near and far')},
        {id: 'terrainAoNear', section: 'baked', label: L('Наш GI в полную силу до', 'Our GI in full up to'), type: 'slider', def: 30, min: 10, max: 62, step: 2, unit: L('м', 'm'), needs: 'bakedFade', expert: true,
         ini: ['Env', 'CRYMT_GI_TERRAINAONEAR'], codec: num(30), hint: L('До этого расстояния только наш GI; дальше до 2,5× него GI уходит, а запечённое затенение нарастает.', 'Up to here our GI alone; from here to 2.5x it the GI fades out and the baked occlusion comes in.'), affects: L('переход вблизи/вдали', 'near/far transition')},

        // ---- engine
        {id: 'zown', section: 'engine', label: L('Свой z-проход', 'Own z-prepass'), type: 'toggle', def: true, expert: true, needs: 'dx12Mode',
         ini: [['Env', 'CRYMT_ZOWN'], ['Env', 'CRYMT_ZPACKETS'], ['Env', 'CRYMT_ZGROUPS'], ['Env', 'CRYMT_ZGROUPRELAX']],
         codec: {write: v => (v ? ['1', '1', 'all', '1'] : [null, null, null, null]), read: a => t(a[0]) != null},
         hint: L('Проход глубины записывается вне потока рендера, объекты одного меша — одной пачкой. Выключать только при проблемах (строки удаляются: значение 0 не выключает).', 'The depth pass is recorded off the render thread, objects of one mesh in one batch. Turn off only if there are problems (the lines are removed: a value of 0 does not turn it off).'), affects: L('FPS, нагрузка на поток рендера', 'FPS, render thread load')},
        {id: 'texrelax', section: 'engine', label: L('Пачки с разными текстурами', 'Batches with different textures'), type: 'toggle', def: true, expert: true, needs: 'dx12Mode',
         ini: ['Env', 'CRYMT_TEXRELAX'], codec: present,
         hint: L('Объекты одного меша, отличающиеся только текстурой, рисуются одной пачкой (≈ 1 мс потока рендера на harbor).', 'Objects of one mesh that differ only in texture are drawn as one batch (≈ 1 ms of render thread on harbor).'), affects: L('FPS в плотных сценах', 'FPS in dense scenes')},
        {id: 'devmode', section: 'engine', label: L('Режим разработчика (-DEVMODE)', 'Developer mode (-DEVMODE)'), type: 'toggle', def: false, expert: true, personal: true,
         ini: ['Launcher', 'DevMode'], codec: defOff,
         hint: L('Запуск с -DEVMODE: консоль (~) и отладочные F-клавиши (F2 — следующая точка, F3 — полёт). GI и клавиши CryMT работают и без него. Движок в этом режиме пишет XML-копию каждого сохранения — CryMT её выключает.', 'Starts with -DEVMODE: the console (~) and the debug F-keys (F2 next checkpoint, F3 fly). GI and the CryMT keys work without it. In this mode the engine writes an XML copy of every save; CryMT turns that off.'),
         affects: L('консоль, отладка', 'console, debugging')},
        {id: 'gsmCache', section: 'engine', label: L('Кэш дальних теней', 'Far shadow cache'), type: 'toggle', def: true,
         ini: ['CVars', 'e_gsm_cache'], codec: defOn, hotkey: L('Num/ ▸ Движок', 'Num/ ▸ Engine'),
         hint: L('Дальние каскады теней перерисовываются реже (e_gsm_cache: в пресете High движка включён, в Very High — нет). Подготовка теней на 25% дешевле, ≈ +3–5% FPS. Если дальние тени запаздывают при резких поворотах — выключите.', 'Far shadow cascades are redrawn less often (e_gsm_cache: on in the engine\'s High spec, off in Very High). Shadow prep 25% cheaper, ≈ +3-5% FPS. If far shadows lag on fast turns, turn it off.'),
         affects: L('FPS, дальние тени', 'FPS, far shadows')},
        {id: 'cbuffer', section: 'engine', label: L('Буфер перекрытий движка', 'Engine coverage buffer'), type: 'toggle', def: false, expert: true,
         ini: ['CVars', 'e_cbuffer'], codec: defOn,
         hint: L('Отсечение объектов, закрытых другими. Выключен из-за гонки с многопоточностью (мигали кусты вдали); на скорость почти не влияет.', 'Culling of objects hidden behind others. Off because of a race with multithreading (distant bushes flickered); it barely changes speed.'), affects: L('мигание дальних кустов', 'distant bushes flicker')},
        {id: 'sprites', section: 'engine', label: L('Дистанция спрайтов деревьев', 'Tree sprite distance'), type: 'slider', def: 1.5, min: 0.3, max: 2, step: 0.1, expert: true,
         ini: ['CVars', 'e_vegetation_sprites_distance_ratio'], codec: num(1.5),
         hint: L('Когда дальние деревья становятся плоскими спрайтами. Меньше — раньше и быстрее (0.3: на 35% меньше вершин), но деревья плоские ближе к камере. Very High = 1.5.', 'When distant trees turn into flat sprites. Lower is earlier and faster (0.3: 35% fewer vertices), but flat trees come closer. Very High = 1.5.'),
         affects: L('треугольники, FPS', 'triangles, FPS')},
    ];

    const PRESETS = [
        {id: 'vanilla', label: L('Ваниль', 'Vanilla'), tag: L('Оригинал', 'Original'), shaders: L('нет', 'none'), cost: '—',
         text: L('Игра как есть. CryMT не загружается.', 'The game as it is. CryMT is not loaded.'), values: {mode: 'off', gi: false}},
        {id: 'mt', label: L('Только MT', 'MT only'), tag: L('Производительность', 'Performance'), shaders: L('нет', 'none'), cost: '+FPS',
         text: L('Многопоточность на стоковом рендере. Ничего в картинке не меняется — подходит для своих шейдеров и модов.', 'Multithreading on the stock renderer. Nothing in the image changes — fine for your own shaders and mods.'), values: {mode: 'mt', gi: false}},
        {id: 'dx12', label: 'MT + DX12', tag: L('Производительность', 'Performance'), shaders: L('нет', 'none'), cost: '+FPS',
         text: L('Наш рендер DirectX 12 поверх многопоточности. Картинка как в оригинале.', 'Our DirectX 12 renderer on top of multithreading. The image as in the original.'), values: {mode: 'dx12', gi: false}},
        {id: 'dlaa', label: 'MT + DLAA', tag: L('Чистая картинка', 'Clean image'), shaders: L('нет', 'none'), cost: L('≈ 2 мс', '≈ 2 ms'),
         text: L('Сглаживание NVIDIA DLAA без ступенек и мерцания листвы. Нужна RTX.', 'NVIDIA DLAA anti-aliasing without jaggies or foliage shimmer. Needs an RTX card.'), values: {mode: 'dlaa', gi: false}},
        {id: 'full', label: L('Полный', 'Full'), tag: L('Всё включено', 'Everything on'), shaders: L('5 файлов', '5 files'), cost: L('≈ 5–6 мс', '≈ 5–6 ms'),
         text: L('DLAA, глобальное освещение, GTAO, контактные тени, освещение дыма.', 'DLAA, global illumination, GTAO, contact shadows, lit smoke.'), values: {mode: 'dlaa', gi: true}},
    ];

    // The author's measurements shown on the preset cards (tools\prof\preset_bench.ps1, second of two passes; filled
    // in by tools\prof\bench_fill.py). fps[preset][test] = [average, minimum].
    const BENCH = {
        system: L('Ryzen 7 5800X · RTX 4070 SUPER · 32 ГБ · 2560×1440 в окне · Very High', 'Ryzen 7 5800X · RTX 4070 SUPER · 32 GB · 2560×1440 window · Very High'),
        tests: [{id: 'cpu2', label: 'CPU2', hint: L('ice: ИИ, физика, эффекты', 'ice: AI, physics, effects')},
                {id: 'gpu', label: 'GPU', hint: L('island: видеокарта', 'island: graphics card')},
                {id: 'village', label: 'Village', hint: L('village: как в игре', 'village: like play')},
                {id: 'harbor', label: 'Harbor', hint: L('harbor: много объектов и ИИ', 'harbor: many objects and AI')}],
        fps: {"vanilla": {"cpu2": [71.1, 0], "gpu": [121.3, 64.7], "village": [83.2, 41.6], "harbor": [79.1, 42.6]}, "mt": {"cpu2": [119.3, 62.0], "gpu": [176.4, 78.9], "village": [110.8, 59.9], "harbor": [89.0, 52.0]}, "dx12": {"cpu2": [126.3, 58.6], "gpu": [168.8, 132.1], "village": [108.7, 51.7], "harbor": [109.9, 71.3]}, "dlaa": {"cpu2": [122.5, 64.8], "village": [104.6, 57.4], "harbor": [102.2, 67.6], "gpu": [142.3, 99.6]}, "full": {"cpu2": [93.6, 66.7], "gpu": [128.0, 69.8], "village": [90.5, 40.6], "harbor": [91.1, 55.2]}}, // BENCH-DATA
    };

    // In-game keys (CryMT's panel; changes last until the game exits, the launcher keeps the lasting ones).
    // [key, what it does, option id it toggles live (optional)]
    const HOTKEYS = [
        {group: L('Освещение (GI)', 'Lighting (GI)'), keys: [
            ['Num0', L('GI вкл / выкл', 'GI on / off'), 'gi'], ['Num1', L('Экран + воксели / только воксели', 'Screen + voxels / voxels only')], ['Num3', L('Геометрия в вокселях', 'Geometry in voxels'), 'geometry'],
            ['Num4 / Num7', L('Затенение в помещениях − / +', 'Indoor occlusion − / +')], ['Num5 / Num8', L('Отражённый свет − / +', 'Bounced light − / +'), 'giBounce'],
            ['Num.', L('GI на оружии и руках', 'GI on weapon and hands'), 'weaponGi'], ['Num*', L('Сброс сил к значениям из настроек', 'Reset strengths to the settings')]]},
        {group: L('Затенение и тени', 'Occlusion and shadows'), keys: [
            ['PgDn', L('GTAO / SSAO движка', 'GTAO / engine SSAO'), 'gtao'], ['Home', L('Контактные тени', 'Contact shadows'), 'contact'],
            ['6', L('Запечённое: RAM', 'Baked: RAM')], ['8', L('Запечённое: небо', 'Baked: sky')], ['9', L('Запечённое: под деревьями (вблизи от GI)', 'Baked: under trees (near ones from GI)'), 'bakedTerrainAo'], ['0', L('Запечённое: вершинное AO террейна', 'Baked: terrain vertex AO')]]},
        {group: L('Дым и частицы', 'Smoke and particles'), keys: [['-', L('GI для дыма и пыли', 'GI for smoke and dust'), 'particles'], ['=', L('Затемнение на улице: 50 / 70 / 100 / 150 / 200 %', 'Darkening outdoors: 50 / 70 / 100 / 150 / 200 %'), 'particleOut']]},
        {group: L('Сглаживание', 'Anti-aliasing'), keys: [[']', L('Стабилизация DLAA на месте', 'DLAA stable on a still camera'), 'fixCopies']]},
        {group: L('Время суток', 'Time of day'), keys: [['Num6', L('Сброс времени суток', 'Reset time of day')], ['Num9', L('Быстрое время суток', 'Fast time of day')], ['Num+ / Num−', L('Время ± 1 час', 'Time ± 1 hour')]]},
        {group: L('Отладка', 'Debugging'), keys: [['Num2', L('Вид вокселей: выкл / каркас / объём', 'Voxel view: off / wire / solid')], ['Num/', L('Скрыть панель CryMT', 'Hide the CryMT panel')], ['Ctrl+F11', L('Оверлей производительности', 'Performance overlay'), 'overlay'], ['PgUp', L('Снимок 8 кадров для разбора ошибок', 'Capture 8 frames for bug reports')]]},
    ];

    // Lines CryMT.ini always gets.
    const ALWAYS = [
        ['CVars', 'pp_RSFDebugWrite', '0', L('-DEVMODE пишет XML-копию каждого сохранения; выключено', '-DEVMODE writes an XML copy of every save; off')],
        ['CVars', 'pp_RSFDebugWriteOnLoad', '0', null],
    ];

    const NEWS = [
        {date: '06.11', title: L('Дрожание DLAA на месте — исправлено', 'DLAA shimmer on a still camera — fixed'), text: L('Пост-обработка игры читала копии сцены до DLAA. Теперь они тоже сглажены.', 'The game\'s post-processing read copies of the scene made before DLAA. Now they are anti-aliased too.')},
        {date: '06.11', title: L('Дым и пыль в помещениях', 'Smoke and dust indoors'), text: L('Частицы берут освещение в своей точке пространства: темнеют в комнатах и под кронами, не мигают.', 'Particles take the lighting at their own point in space: they darken in rooms and under crowns, without flicker.')},
        {date: '06.11', title: L('Бинокль', 'Binoculars'), text: L('Второй зум больше не замораживает GTAO и снова сглаживается.', 'The second zoom no longer freezes GTAO and is anti-aliased again.')},
        {date: '05.11', title: L('Контактные тени и visibility bitmask', 'Contact shadows and visibility bitmask'), text: L('Тени у основания предметов; экранный GI без свечения углов.', 'Shadows at the base of objects; screen GI without glowing corners.')},
    ];

    // Where the launcher looks for new builds (GitHub releases with manifest.txt, manifest.sig and a .zip).
    const UPDATE = {
        repo: 'yakimowivan-oss/CryMT-Releases',
        api: 'https://api.github.com/repos/yakimowivan-oss/CryMT-Releases/releases?per_page=20',
    };

    // Filled by the launcher window from the game folder; these are the values a plain browser preview shows.
    const STATUS = {
        game: 'D:\\Games\\Crysis', build: '6156', buildOk: true, launcher: 'c1-launcher v8', launcherOk: true,
        installed: '', installedDate: '', channel: 'stable',
        mods: [], gpu: '', dlss: true, editor: false, // <game>\Mods: [{name, dll, dll32, shaders, gi}]; the graphics card
    };

    return {SECTIONS, OPTIONS, PRESETS, HOTKEYS, ALWAYS, NEWS, STATUS, UPDATE, BENCH};
})();
