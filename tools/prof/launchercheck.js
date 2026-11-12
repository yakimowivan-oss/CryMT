// Launcher vs code check: node tools\prof\launchercheck.js [CryMT.ini]
// 1. CRYMT_* variables the DLL / proxy read against the ones the launcher knows (options.js), and the code's default
//    of each number knob against the launcher's codec (num(d)).
// 2. CryMT.ini round trip: reading the installed file and saving it unchanged must give the same bytes; changing one
//    knob must change exactly its lines.
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const repo = path.resolve(__dirname, '..', '..');
const ui = path.join(repo, 'src', 'launcher', 'ui');
const ctx = {window: {}};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(ui, 'options.js'), 'utf8'), ctx);
vm.runInContext(fs.readFileSync(path.join(ui, 'ini.js'), 'utf8'), ctx);
const C = ctx.window.CRYMT, I = ctx.window.CryIni;

// Variables that are tests, logs or captures: not for players.
const INTERNAL = /^CRYMT_(CAPTURE_|.*CENSUS|.*VERIFY|.*TRACE|.*DUMP$|.*LOG$|PSOLOG|GITEST|GI_DEBUG|GI_TIMESWEEP|GI_LAMPTEST|TESTCMD|STARTCMD|DRED|SFQUEUE|BFIDENTITY|PHYSTURN|PARTICLERAYS|RECORDKEYS|FRAMELOG|HEARTBEAT|D3D12DEBUG|DLSS_NOEVAL|DEVGATE|ZCYCLES|ZTRACE|SPRITECHECK|SHADOWREC|GPUPROF|SUNDUMP|VISDUMP|PAKLOG)/;
// Knobs known and kept out of the launcher on purpose (experiments, comparisons, fixed tuning): a new variable in the
// code shows up below until it gets an option in options.js or a place here.
const HIDDEN = new Set([
    'ASYNCPSO', 'ASYNCSAVE', 'BUFFERS', 'COMPILEFENCE', 'D3DWRAP', 'DLSS_AUTOEXPOSURE', 'DLSS_CLAMP',
    'DLSS_EXPOSURE', 'DLSS_EXPOSUREKEY', 'DLSS_EXPOSURESCALE', 'DLSS_JITTER', 'DLSS_MVSCALE', 'EARLYBARRIER',
    'FENCE', 'FENCEAT', 'FRUSTUMTEST', 'GI_CONELIFT', 'GI_CONESTART', 'GI_DIRECTIONS', 'GI_DISTANCE', 'GI_FIREFLY',
    'GI_FIXEDNOISE', 'GI_FLOOR', 'GI_FOLIAGE', 'GI_LAMPS', 'GI_MATERIALS', 'GI_MIPOPACITY', 'GI_NARROWNORMALS',
    'GI_NEAREST', 'GI_NEAROCC', 'GI_OCCRANGE', 'GI_OCCSTRENGTH', 'GI_PARTICLECELL', 'GI_ROOFCELLS', 'GI_SCALE',
    'GI_SHADOWGEOM', 'GI_SKYFILL', 'GI_STARTTIME', 'GI_SUNALBEDO', 'GI_THIN', 'GI_TIMESPEED', 'GI_UPSAMPLE',
    'GI_VOXELREACH', 'GI_VOXELSIZE', 'GI_VOXELVIEW', 'LOOPGROUPS', 'MIPBIAS', 'MOTION', 'NODEFERFREE', 'NOPROXY',
    'NOPUMP', 'PIFAST', 'PROCBUDGET', 'SCENESYNC', 'SETREUSE', 'SPRITEFIX', 'SYNC_BEGINFRAME', 'TERRAINFENCE',
    'TERRAINTEX', 'WARMUP', 'WORLDSYNC', 'ZBUCKET', 'ZGROUPVALUES', 'ZOWNDRAW', 'ZOWNSPLIT', 'MARKERS', 'RENDERDOC', 'SAFEPOOL_MB', 'MEMFIX', 'ENVMAP', 'ENVTEST', 'RENDERDOC_AT', 'RENDERDOC_AT2', 'ASYNCPRESENT', 'DIRECTMAPS', 'ASYNCQUERIES', 'ASYNCTEXMAP', 'PRESENTQUEUE', 'GPUPASSES', 'EARLYFENCE', 'SLICEMARGIN', 'EARLYIDLE', 'LEVELWATCH', 'THREADLEVEL', 'TODJUMPS', 'STREAMSLOW', 'DIRECTNOOVERWRITE', 'QUERYWAIT', 'RENDERDOC_FRAME', 'EVENTSYNC', 'CASTERCENSUS', 'EARLYAIFENCE', 'FPLUS',
    'FILLLIGHTQ', 'Q17', 'SSS', 'SSS_CONTRAST', 'SSS_EDGE', 'SSS_NEAR', 'SSS_REACH', 'SSS_SAMPLES', 'SSS_THICKNESS', 'SSS_WEAPONTHICK', 'WATERCUBE', 'WATERSKY', 'ENVCUBE', 'ENVCUBE_FLAGS', 'ENVCUBE_JUMP', 'ENVCUBE_RATE', 'ENVCUBE_ROLL1', 'ENVCUBE_SCALE', 'ENVCUBE_CULL', 'ENVCUBE_BLEND', 'ENVCUBE_DOWN'
].map(k => 'CRYMT_' + k));
// Set by the proxy from [CryMT] Mode / GI / Borderless / PauseOnFocusLoss.
const FROM_MODE = new Set(['CRYMT_MODE', 'CRYMT_LEVEL', 'CRYMT_FILL', 'CRYMT_TERRAINOWN', 'CRYMT_D3D12', 'CRYMT_D3DSTREAM', 'CRYMT_DLSS',
                           'CRYMT_GI', 'CRYMT_BORDERLESS', 'CRYMT_PAUSEONFOCUS']);

const src = path.join(repo, 'src', 'crymt');
const files = ['CryMT.cpp', 'D3D12Backend.cpp', 'D3DLayer.cpp', 'Dlss.cpp', 'ZPass.cpp', 'Window.cpp', 'RenderDocCapture.cpp', 'MemoryFix.cpp'].map(f => fs.readFileSync(path.join(src, f), 'utf8'));
const code = new Set();
files.forEach(t => (t.match(/"CRYMT_[A-Z0-9_]+"/g) || []).forEach(m => code.add(m.slice(1, -1))));
const launcher = new Map(); // key -> option
C.OPTIONS.forEach(o => I.keysOf(o).forEach(([sec, key]) => { if (sec === 'Env') launcher.set(key, o); }));

const missing = [...code].filter(k => !launcher.has(k) && !INTERNAL.test(k) && !FROM_MODE.has(k) && !HIDDEN.has(k)).sort();
const unknown = [...launcher.keys()].filter(k => !code.has(k)).sort();
console.log('Переменные: в коде ' + code.size + ', в лаунчере ' + launcher.size + ', отладочных ' + [...code].filter(k => INTERNAL.test(k)).length + ', скрытых намеренно ' + HIDDEN.size);
console.log('  в коде, но нет в лаунчере (' + missing.length + '): ' + (missing.join(' ') || '-'));
console.log('  в лаунчере, но код не читает (' + unknown.length + '): ' + (unknown.join(' ') || '-'));

// Defaults: read(L"CRYMT_X", v.member) + "member = 1.5f" in the GI settings struct.
const backend = files[1];
let checked = 0;
const wrong = [];
for (const m of backend.matchAll(/read\(L"(CRYMT_[A-Z0-9_]+)",\s*v\.(\w+)\)/g)) {
    const o = launcher.get(m[1]);
    if (!o || !o.codec.code && o.codec.code !== 0) continue;
    const d = backend.match(new RegExp('\\b' + m[2] + '\\s*=\\s*(-?[\\d.]+)f?\\s*[,;]'));
    if (!d) { wrong.push(m[1] + ': нет значения по умолчанию в коде'); continue; }
    checked++;
    if (Math.abs(parseFloat(d[1]) - o.codec.code) > 1e-6) wrong.push(m[1] + ': код ' + d[1] + ', лаунчер ' + o.codec.code);
}
console.log('Значения по умолчанию: сверено ' + checked + ', расхождений ' + wrong.length + (wrong.length ? '\n  ' + wrong.join('\n  ') : ''));

// Round trip on the installed ini.
const iniPath = process.argv[2] || 'D:\\Games\\Crysis\\CryMT\\CryMT.ini';
const text = fs.readFileSync(iniPath, 'utf8');
const values = I.readValues(C, text);
const same = I.merge(C, text, values);
const lines = t => t.split(/\r?\n/);
const diff = (a, b) => {
    const x = lines(a), y = lines(b), out = [];
    const sx = new Set(x), sy = new Set(y);
    x.forEach(l => { if (!sy.has(l)) out.push('- ' + l); });
    y.forEach(l => { if (!sx.has(l)) out.push('+ ' + l); });
    return out;
};
console.log('CryMT.ini (' + lines(text).length + ' строк): сохранение без изменений ' + (same === text ? 'даёт тот же файл' : 'МЕНЯЕТ файл:\n  ' + diff(text, same).join('\n  ')));
const tries = [['giBounce', 5], ['zown', !values.zown], ['geometry', values.geometry === 'off' ? 'on' : 'off'], ['keys', !values.keys], ['devmode', !values.devmode]];
for (const [k, v] of tries) {
    const t = I.merge(C, text, Object.assign({}, values, {[k]: v}));
    const d = diff(text, t);
    const back = I.readValues(C, t)[k];
    console.log('  ' + k + ' = ' + JSON.stringify(v) + ': ' + d.length + ' строк(и) изменено [' + d.join(' | ') + '], читается обратно: ' + (I.same(back, v) ? 'да' : 'НЕТ (' + back + ')'));
}
const defs = {};
C.OPTIONS.forEach(o => (defs[o.id] = o.def));
const f = I.fresh(C, defs);
const r = I.readValues(C, f);
const off = C.OPTIONS.filter(o => !I.same(r[o.id], o.def)).map(o => o.id);
console.log('Новый ini с нуля: ' + lines(f).length + ' строк, значения лаунчера читаются обратно: ' + (off.length ? 'НЕТ: ' + off.join(' ') : 'все ' + C.OPTIONS.length));
