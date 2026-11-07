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
//   ms        v -> rough GPU milliseconds a frame (village, 5120x1440) for the load meter
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

    const SECTIONS = [
        {id: 'mode', label: 'Режим'},
        {id: 'aa', label: 'Сглаживание'},
        {id: 'gi', label: 'Освещение (GI)'},
        {id: 'ao', label: 'Затенение и тени'},
        {id: 'particles', label: 'Дым и частицы'},
        {id: 'baked', label: 'Запечённое затенение'},
        {id: 'engine', label: 'Движок'},
    ];

    const OPTIONS = [
        // ---- mode
        {id: 'mode', section: 'mode', label: 'Режим работы', type: 'select', def: 'dlaa',
         choices: [{v: 'off', label: 'Ваниль'}, {v: 'mt', label: 'Многопоточность'}, {v: 'dx12', label: 'MT + DirectX 12'}, {v: 'dlaa', label: 'MT + DX12 + DLAA'}],
         ini: ['CryMT', 'Mode'], codec: {write: v => v, read: s => (t(s) || 'mt').toLowerCase()},
         hint: 'Многопоточность переносит отрисовку в свой поток: игра упирается в процессор меньше. DirectX 12 заменяет рендер игры на наш. DLAA — сглаживание NVIDIA (нужна видеокарта RTX).',
         affects: 'FPS, плавность, сглаживание', cost: 'MT: +FPS · DLAA ≈ 2.2 мс', ms: v => (v === 'dlaa' ? 2.2 : 0)},
        {id: 'borderless', section: 'mode', label: 'Окно без рамки', type: 'toggle', def: true, ini: ['CryMT', 'Borderless'], codec: flag(true), personal: true,
         hint: 'Полноэкранное окно без рамки: быстрое переключение Alt+Tab, без смены видеорежима.', affects: 'окно игры'},
        {id: 'pause', section: 'mode', label: 'Пауза при сворачивании', type: 'toggle', def: true, ini: ['CryMT', 'PauseOnFocusLoss'], codec: flag(true), personal: true,
         hint: 'Игра встаёт на паузу, когда окно не в фокусе.', affects: 'поведение окна'},
        {id: 'keys', section: 'mode', label: 'Клавиши CryMT в игре', type: 'toggle', def: true, needs: 'dx12Mode',
         ini: ['Env', 'CRYMT_GI_KEYS'], codec: defOff, personal: true,
         hint: 'Num-блок (с NumLock) и несколько клавиш основной клавиатуры переключают функции на лету — сравнить «было / стало». Список — во вкладке «Клавиши». Изменения действуют до выхода из игры.',
         affects: 'управление в игре'},
        {id: 'overlay', section: 'mode', label: 'Оверлей производительности', type: 'toggle', def: false, personal: true, needs: 'mtMode', hotkey: 'Ctrl+F11',
         ini: ['Env', 'CRYMT_OVERLAY'], codec: present,
         hint: 'Время кадра, загрузка потоков и видеокарты поверх игры. Включается и выключается в игре клавишами Ctrl+F11.', affects: 'оверлей поверх игры'},
        {id: 'afterStart', section: 'mode', label: 'После запуска игры', type: 'select', def: 'minimize', personal: true,
         choices: [{v: 'minimize', label: 'Свернуть'}, {v: 'close', label: 'Закрыть'}, {v: 'stay', label: 'Оставить'}],
         ini: ['Launcher', 'AfterStart'], codec: pick({minimize: null, close: 'close', stay: 'stay'}),
         hint: 'Что делает лаунчер, когда игра запущена.', affects: 'окно лаунчера'},

        // ---- anti-aliasing
        {id: 'fixCopies', section: 'aa', label: 'Стабилизация DLAA на месте', type: 'toggle', def: true, ini: ['Env', 'CRYMT_DLSS_FIXCOPIES'], codec: defOn, hotkey: ']',
         hint: 'Пост-обработка игры читает копии сцены, сделанные до DLAA (с джиттером камеры). Без этого картинка на стоящей камере дрожала по листве и мелким деталям.',
         affects: 'дрожание на стоящей камере', cost: '≈ 0.05 мс', needs: 'dlaaMode', ms: v => (v ? 0.05 : 0)},
        {id: 'preset', section: 'aa', label: 'Модель DLAA', type: 'select', def: 'auto', expert: true, needs: 'dlaaMode',
         choices: [{v: 'auto', label: 'По умолчанию (K)'}, {v: 'K', label: 'K — трансформер'}, {v: 'J', label: 'J'}, {v: 'F', label: 'F — CNN'}, {v: 'E', label: 'E — CNN'}],
         ini: ['Env', 'CRYMT_DLSS_PRESET'], codec: pick({auto: null, K: 'K', J: 'J', F: 'F', E: 'E'}),
         hint: 'Нейросеть DLAA. Трансформер (K) лучше держит листву и мелкие детали; старые CNN (E/F) мягче и дешевле.', affects: 'резкость, шлейфы'},

        // ---- GI
        {id: 'gi', section: 'gi', label: 'Глобальное освещение', type: 'toggle', def: true, ini: ['CryMT', 'GI'], codec: flag(false), shaders: true, hotkey: 'Num0', needs: 'dx12Mode',
         hint: 'Отражённый свет и затенение от окружения: стены и потолки затеняют комнаты, освещённые солнцем поверхности подсвечивают соседние, кроны затемняют землю. Работает в режиме DirectX 12.',
         affects: 'освещение помещений, углов, под деревьями', cost: '≈ 1.5 мс основа, всё вместе до 3.6', ms: v => (v ? 1.5 : 0)},
        {id: 'giRadius', section: 'gi', label: 'Радиус экранной части', type: 'slider', def: 0.75, min: 0.25, max: 3, step: 0.05, unit: 'м', needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_RADIUS'], codec: num(2.5),
         hint: 'Как далеко по экрану ищется отражённый свет у каждой точки. Больше — мягче и шире, но больше шума.', affects: 'размер подсветки у поверхностей'},
        {id: 'giBounce', section: 'gi', label: 'Сила отскока (экран)', type: 'slider', def: 6.7, min: 0, max: 10, step: 0.1, needs: 'gi', hotkey: 'Num5 / Num8',
         ini: ['Env', 'CRYMT_GI_BOUNCE'], codec: num(1.0),
         hint: 'Насколько ярко освещённые поверхности на экране подсвечивают соседние. Слишком большое значение высветляет углы комнат.', affects: 'цветной отражённый свет'},
        {id: 'giAo', section: 'gi', label: 'Затенение (экран)', type: 'slider', def: 4, min: 0, max: 8, step: 0.1, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_AO'], codec: num(0.8),
         hint: 'Сила экранной части затенения окружающего света (поверх вокселей).', affects: 'тёмность углов и под предметами'},
        {id: 'voxels', section: 'gi', label: 'Воксели вокруг камеры', type: 'toggle', def: true, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_VOXELS'], codec: defOn, cost: '≈ 1 мс',
         hint: 'Сетки 0.25–2 м до 256 м вокруг камеры: свет и затенение от того, чего нет на экране (крыши, стены за спиной). Без них GI — только экранный.', affects: 'помещения, крыши, кроны'},
        {id: 'voxStrength', section: 'gi', label: 'Сила отскока (воксели)', type: 'slider', def: 3, min: 0, max: 5, step: 0.1, needs: 'gi',
         ini: ['Env', 'CRYMT_GI_VOXELSTRENGTH'], codec: num(1.5),
         hint: 'Отражённый свет от геометрии вокруг, в том числе вне экрана (солнце и лампы на стенах, полу, крышах).', affects: 'общая подсветка сцены'},
        {id: 'voxAo', section: 'gi', label: 'Затенение от вокселей', type: 'slider', def: 1.2, min: 0, max: 2, step: 0.05, needs: 'gi',
         ini: ['Env', 'CRYMT_GI_VOXELAO'], codec: num(0.6),
         hint: 'Насколько окружение (стены, потолок, кроны) закрывает небо. Главный источник затемнения помещений.', affects: 'тёмность помещений и под деревьями'},
        {id: 'giOutdoor', section: 'gi', label: 'Затенение на улице', type: 'slider', def: 1.0, min: 0, max: 2, step: 0.05, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_OUTDOOR'], codec: num(1.0),
         hint: 'Сила затенения вне помещений (VisArea). В помещениях — своя сила (Num4/Num7 в игре).', affects: 'улица'},
        {id: 'giTemporal', section: 'gi', label: 'Накопление по кадрам', type: 'slider', def: 0.7, min: 0, max: 0.95, step: 0.05, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_TEMPORAL'], codec: num(0.75),
         hint: 'Смешивание GI с прошлыми кадрами. Выше — спокойнее картинка, но дольше «догоняет» при движении.', affects: 'шум и шлейфы GI'},
        {id: 'geometry', section: 'gi', label: 'Геометрия в вокселях', type: 'select', def: 'on', needs: 'gi', hotkey: 'Num3',
         choices: [{v: 'off', label: 'Только видимое'}, {v: 'on', label: 'Вся геометрия'}, {v: 'foliage', label: 'Вся + листва целиком'}],
         ini: ['Env', 'CRYMT_GI_GEOMETRY'], codec: pick({off: '0', on: null, foliage: '2'}), cost: '≈ 0.6 мс', ms: v => (v === 'off' ? 0 : 0.6),
         hint: 'Без неё воксели знают только то, что видит камера. С ней — всю геометрию сцены: кроны над головой и крыши затеняют точнее, но полы в помещениях темнее и при входе через портал освещение пересчитывается.',
         affects: 'точность затенения, порталы'},
        {id: 'geomKeep', section: 'gi', label: 'Хранить вокселизированную геометрию', type: 'toggle', def: true, needs: 'geometry', expert: true,
         ini: ['Env', 'CRYMT_GI_GEOMPERSIST'], codec: defOn,
         hint: 'Геометрия остаётся в вокселях, пока экран не покажет, что место пусто. Без этого она пропадает через 3 кадра после того, как движок перестал её рисовать.', affects: 'стабильность GI'},
        {id: 'foliageGi', section: 'gi', label: 'GI на траве и листве', type: 'slider', def: 1.5, min: 1, max: 2.5, step: 0.1, unit: '×', needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_FOLIAGEGI'], codec: num(1.5),
         hint: 'Усиление затенения и отскока на растительности.', affects: 'трава, кусты'},
        {id: 'screenClassic', section: 'gi', label: 'Старый метод экранного GI', type: 'toggle', def: false, needs: 'gi', expert: true,
         ini: ['Env', 'CRYMT_GI_SCREENCLASSIC'], codec: defOff,
         hint: 'Горизонтный метод вместо visibility bitmask. Для сравнения: старый сильнее подсвечивал углы комнат.', affects: 'углы помещений'},
        {id: 'weaponGi', section: 'gi', label: 'GI на оружии и руках', type: 'toggle', def: true, needs: 'gi', hotkey: 'Num.', expert: true,
         ini: ['Env', 'CRYMT_GI_WEAPON'], codec: defOn,
         hint: 'Оружие, руки и предметы в руках затеняются и подсвечиваются окружением, как остальная сцена.', affects: 'вид от первого лица'},

        // ---- AO and shadows
        {id: 'gtao', section: 'ao', label: 'Затенение в складках', type: 'select', def: 'gtao', offValue: 'ssao', needs: 'gi', hotkey: 'PgDn',
         choices: [{v: 'gtao', label: 'GTAO (наше)'}, {v: 'ssao', label: 'SSAO движка'}],
         ini: ['Env', 'CRYMT_GI_SSAO'], codec: pick({gtao: null, ssao: '1'}), cost: 'GTAO ≈ 1.2 мс', ms: v => (v === 'gtao' ? 1.2 : 0),
         hint: 'GTAO точнее и темнее в щелях и углах; SSAO движка мягче и привычнее (так задумывали художники).', affects: 'складки, углы, контакт предметов'},
        {id: 'gtaoStrength', section: 'ao', label: 'Сила GTAO', type: 'slider', def: 1.5, min: 0.5, max: 3, step: 0.1, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAO'], codec: num(1.5), hint: 'Насколько темнеют складки.', affects: 'тёмность складок'},
        {id: 'gtaoRadius', section: 'ao', label: 'Радиус GTAO', type: 'slider', def: 0.5, min: 0.2, max: 2, step: 0.05, unit: 'м', needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAORADIUS'], codec: num(0.5), hint: 'Размер затенения вокруг складок.', affects: 'ширина тени в углах'},
        {id: 'gtaoFull', section: 'ao', label: 'GTAO в полном разрешении', type: 'toggle', def: false, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFULL'], codec: defOff, cost: '+3.3 мс', ms: v => (v ? 3.3 : 0),
         hint: 'Резче, но заметно дороже. По умолчанию — половинное разрешение.', affects: 'резкость затенения'},
        {id: 'gtaoHistory', section: 'ao', label: 'Накопление GTAO по кадрам', type: 'toggle', def: false, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOHISTORY'], codec: defOff,
         hint: 'Меньше шума, но при движении затенение «плывёт» и мылится. По умолчанию выключено: фиксированный узор без истории.', affects: 'шум и шлейфы'},
        {id: 'gtaoFoliage', section: 'ao', label: 'GTAO на растениях', type: 'toggle', def: true, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFOLIAGE'], codec: defOn,
         hint: 'Трава и листва тоже получают GTAO. Без него растения плоские (проверено — хуже).', affects: 'трава, кусты'},
        {id: 'gtaoFoliageShade', section: 'ao', label: 'Мягче на растениях в тени', type: 'toggle', def: true, needs: 'gtao', expert: true,
         ini: ['Env', 'CRYMT_GI_GTAOFOLIAGESHADE'], codec: defOn,
         hint: 'На траве и листве в тени солнца GTAO работает на 20% силы: там окружающий свет — весь их свет, и затенение выглядело слишком сильным.', affects: 'растительность в тени'},
        {id: 'contact', section: 'ao', label: 'Контактные тени', type: 'toggle', def: true, needs: 'gi', hotkey: 'Home', shaders: true,
         ini: ['Env', 'CRYMT_CONTACTSHADOWS'], codec: defOn, cost: '≈ 0.1 мс', ms: v => (v ? 0.1 : 0),
         hint: 'Короткие тени от солнца у основания предметов, где карта теней слишком грубая. Трава и листья их не отбрасывают (видно, что трава висит над землёй).',
         affects: 'объём предметов на солнце'},
        {id: 'contactLength', section: 'ao', label: 'Длина контактных теней', type: 'slider', def: 0.35, min: 0.1, max: 1, step: 0.05, unit: 'м', needs: 'contact', expert: true,
         ini: ['Env', 'CRYMT_CONTACTLENGTH'], codec: num(0.35), hint: 'Длина на 10 м от камеры (дальше растёт).', affects: 'длина теней'},
        {id: 'contactFoliage', section: 'ao', label: 'Растения тоже отбрасывают', type: 'toggle', def: false, needs: 'contact', expert: true,
         ini: ['Env', 'CRYMT_CONTACTFOLIAGE'], codec: defOff,
         hint: 'Старое поведение: трава и листья отбрасывают контактные тени (луга темнеют при низком солнце).', affects: 'трава, листва'},

        // ---- particles
        {id: 'particles', section: 'particles', label: 'GI для дыма и пыли', type: 'toggle', def: true, needs: 'gi', hotkey: '-', shaders: true,
         ini: ['Env', 'CRYMT_GI_PARTICLES'], codec: defOn, cost: '≈ 0.3 мс', ms: v => (v ? 0.3 : 0),
         hint: 'Дым, пыль и туман темнеют в помещениях и под кронами, берут оттенок окружения; солнце в комнатах — по вокселям (без мигания). Огонь, искры и вспышки не трогаются.',
         affects: 'дым, пыль, туман'},
        {id: 'particleOut', section: 'particles', label: 'Затемнение на улице', type: 'slider', def: 2.0, min: 0.5, max: 2, step: 0.25, unit: '×', needs: 'particles', hotkey: '=',
         ini: ['Env', 'CRYMT_GI_PARTICLEOUT'], codec: num(2.0), hint: 'Относительно помещений. Лёгкая дымка затемняется меньше, чтобы не пропадала.', affects: 'дым на улице'},
        {id: 'particleMax', section: 'particles', label: 'Предел затемнения', type: 'slider', def: 0.85, min: 0.4, max: 0.95, step: 0.05, needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLEMAX'], codec: num(0.85), hint: 'Какую долю окружающего света частица может потерять максимум.', affects: 'самые тёмные места'},
        {id: 'particleTint', section: 'particles', label: 'Оттенок окружения', type: 'slider', def: 0.5, min: 0, max: 1, step: 0.05, needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLETINT'], codec: num(0.5), hint: 'Насколько дым окрашивается в цвет света вокруг (зелень под кронами, тёплый свет у стен). Яркость не меняется.', affects: 'цвет дыма'},
        {id: 'particleFine', section: 'particles', label: 'Точность у стен', type: 'slider', def: 0.5, min: 0.25, max: 1, step: 0.05, unit: 'м', needs: 'particles', expert: true,
         ini: ['Env', 'CRYMT_GI_PARTICLEFINE'], codec: num(0.5), hint: 'Ячейка мелкого объёма (±32 ячейки вокруг камеры). Меньше — точнее у стен и потолков, но меньше охват.', affects: 'пыль у стен'},

        // ---- baked
        {id: 'bakedTerrainAo', section: 'baked', label: 'Затенение под деревьями: вблизи от GI', type: 'toggle', def: false, needs: 'gi', hotkey: '9', expert: true,
         ini: ['Env', 'CRYMT_GI_BAKED'],
         codec: {write: v => (v ? 'terrainao' : null), read: s => t(s) != null && (t(s) === '0' || t(s).toLowerCase().split(',').map(x => x.trim()).includes('terrainao'))},
         hint: 'Движок затеняет землю под кронами по грубой карте (ступеньки на границах клеток). Вблизи (до 30 м) его можно заменить нашим GI, дальше остаётся движковое.', affects: 'земля под деревьями'},
        {id: 'terrainAoNear', section: 'baked', label: 'Дистанция замены', type: 'slider', def: 30, min: 10, max: 62, step: 2, unit: 'м', needs: 'bakedTerrainAo', expert: true,
         ini: ['Env', 'CRYMT_GI_TERRAINAONEAR'], codec: num(30), hint: 'До какого расстояния затенение под деревьями даёт наш GI.', affects: 'переход вблизи/вдали'},

        // ---- engine
        {id: 'zown', section: 'engine', label: 'Свой z-проход', type: 'toggle', def: true, expert: true, needs: 'dx12Mode',
         ini: [['Env', 'CRYMT_ZOWN'], ['Env', 'CRYMT_ZPACKETS'], ['Env', 'CRYMT_ZGROUPS'], ['Env', 'CRYMT_ZGROUPRELAX']],
         codec: {write: v => (v ? ['1', '1', 'all', '1'] : [null, null, null, null]), read: a => t(a[0]) != null},
         hint: 'Проход глубины записывается вне потока рендера, объекты одного меша — одной пачкой. Выключать только при проблемах (строки удаляются: значение 0 не выключает).', affects: 'FPS, нагрузка на поток рендера'},
        {id: 'texrelax', section: 'engine', label: 'Пачки с разными текстурами', type: 'toggle', def: true, expert: true, needs: 'dx12Mode',
         ini: ['Env', 'CRYMT_TEXRELAX'], codec: present,
         hint: 'Объекты одного меша, отличающиеся только текстурой, рисуются одной пачкой (≈ 1 мс потока рендера на harbor).', affects: 'FPS в плотных сценах'},
        {id: 'devmode', section: 'engine', label: 'Режим разработчика (-DEVMODE)', type: 'toggle', def: false, expert: true, personal: true,
         ini: ['Launcher', 'DevMode'], codec: defOff,
         hint: 'Запуск с -DEVMODE: консоль (~) и отладочные F-клавиши (F2 — следующая точка, F3 — полёт). GI и клавиши CryMT работают и без него. Движок в этом режиме пишет XML-копию каждого сохранения — CryMT её выключает.',
         affects: 'консоль, отладка'},
        {id: 'cbuffer', section: 'engine', label: 'Буфер перекрытий движка', type: 'toggle', def: false, expert: true,
         ini: ['CVars', 'e_cbuffer'], codec: defOn,
         hint: 'Отсечение объектов, закрытых другими. Выключен из-за гонки с многопоточностью (мигали кусты вдали); на скорость почти не влияет.', affects: 'мигание дальних кустов'},
        {id: 'sprites', section: 'engine', label: 'Дистанция спрайтов деревьев', type: 'slider', def: 1.5, min: 0.3, max: 2, step: 0.1, expert: true,
         ini: ['CVars', 'e_vegetation_sprites_distance_ratio'], codec: num(1.5),
         hint: 'Когда дальние деревья становятся плоскими спрайтами. Меньше — раньше и быстрее (0.3: на 35% меньше вершин), но деревья плоские ближе к камере. Very High = 1.5.',
         affects: 'треугольники, FPS'},
    ];

    const PRESETS = [
        {id: 'vanilla', label: 'Ваниль', tag: 'Оригинал', shaders: 'нет', cost: '—',
         text: 'Игра как есть. CryMT не загружается.', values: {mode: 'off', gi: false}, fps: {village: 55.1, island: 65.2}},
        {id: 'mt', label: 'Только MT', tag: 'Производительность', shaders: 'нет', cost: '+FPS',
         text: 'Многопоточность на стоковом рендере. Ничего в картинке не меняется — подходит для своих шейдеров и модов.', values: {mode: 'mt', gi: false}},
        {id: 'dx12', label: 'MT + DX12', tag: 'Производительность', shaders: 'нет', cost: '+FPS',
         text: 'Наш рендер DirectX 12 поверх многопоточности. Картинка как в оригинале.', values: {mode: 'dx12', gi: false}},
        {id: 'dlaa', label: 'MT + DLAA', tag: 'Чистая картинка', shaders: 'нет', cost: '≈ 2 мс',
         text: 'Сглаживание NVIDIA DLAA без ступенек и мерцания листвы. Нужна RTX.', values: {mode: 'dlaa', gi: false}, fps: {village: 57.0}},
        {id: 'full', label: 'Полный', tag: 'Всё включено', shaders: '5 файлов', cost: '≈ 5–6 мс',
         text: 'DLAA, глобальное освещение, GTAO, контактные тени, освещение дыма.', values: {mode: 'dlaa', gi: true}, fps: {village: 50.2, island: 59.8}},
    ];

    // In-game keys (CryMT's panel; changes last until the game exits, the launcher keeps the lasting ones).
    // [key, what it does, option id it toggles live (optional)]
    const HOTKEYS = [
        {group: 'Освещение (GI)', keys: [
            ['Num0', 'GI вкл / выкл', 'gi'], ['Num1', 'Экран + воксели / только воксели'], ['Num3', 'Геометрия в вокселях', 'geometry'],
            ['Num4 / Num7', 'Затенение в помещениях − / +'], ['Num5 / Num8', 'Отражённый свет − / +', 'giBounce'],
            ['Num.', 'GI на оружии и руках', 'weaponGi'], ['Num*', 'Сброс сил к значениям из настроек']]},
        {group: 'Затенение и тени', keys: [
            ['PgDn', 'GTAO / SSAO движка', 'gtao'], ['Home', 'Контактные тени', 'contact'],
            ['6', 'Запечённое: RAM'], ['8', 'Запечённое: небо'], ['9', 'Запечённое: под деревьями (вблизи от GI)', 'bakedTerrainAo'], ['0', 'Запечённое: вершинное AO террейна']]},
        {group: 'Дым и частицы', keys: [['-', 'GI для дыма и пыли', 'particles'], ['=', 'Затемнение на улице: 50 / 70 / 100 / 150 / 200 %', 'particleOut']]},
        {group: 'Сглаживание', keys: [[']', 'Стабилизация DLAA на месте', 'fixCopies']]},
        {group: 'Время суток', keys: [['Num6', 'Сброс времени суток'], ['Num9', 'Быстрое время суток'], ['Num+ / Num−', 'Время ± 1 час']]},
        {group: 'Отладка', keys: [['Num2', 'Вид вокселей: выкл / каркас / объём'], ['Num/', 'Скрыть панель CryMT'], ['Ctrl+F11', 'Оверлей производительности', 'overlay'], ['PgUp', 'Снимок 8 кадров для разбора ошибок']]},
    ];

    // Lines CryMT.ini always gets.
    const ALWAYS = [
        ['CVars', 'pp_RSFDebugWrite', '0', '-DEVMODE (нужен для GI) пишет XML-копию каждого сохранения; выключено'],
        ['CVars', 'pp_RSFDebugWriteOnLoad', '0', null],
    ];

    const NEWS = [
        {date: '06.11', title: 'Дрожание DLAA на месте — исправлено', text: 'Пост-обработка игры читала копии сцены до DLAA. Теперь они тоже сглажены.'},
        {date: '06.11', title: 'Дым и пыль в помещениях', text: 'Частицы берут освещение в своей точке пространства: темнеют в комнатах и под кронами, не мигают.'},
        {date: '06.11', title: 'Бинокль', text: 'Второй зум больше не замораживает GTAO и снова сглаживается.'},
        {date: '05.11', title: 'Контактные тени и visibility bitmask', text: 'Тени у основания предметов; экранный GI без свечения углов.'},
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
    };

    return {SECTIONS, OPTIONS, PRESETS, HOTKEYS, ALWAYS, NEWS, STATUS, UPDATE};
})();
