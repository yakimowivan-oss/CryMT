// CryMT launcher UI. Everything shown comes from options.js, CryMT.ini goes through ini.js; this file draws and talks to
// the window (CryMTLauncher.exe, WebView2): messages 'load', 'save\n<ini>', 'play\n<gi>\n<args>\n<after>',
// 'editor\n<gi>\n<crymt>\n<args>\n<after>', 'drag', 'min', 'max', 'resize\n<edge>', 'close', 'open\n<what>'; replies {type: 'state' |
// 'saved' | 'played' | 'window' | 'error'}. Opened as a plain page in a
// browser it works on localStorage and shows what the window would do.
(function () {
    'use strict';
    const I = window.CryIni;
    const STORE = 'crymt.launcher.v2';

    // Language. Every text of options.js is a pair L('русский', 'English'); C is options.js with the pairs resolved to
    // the current language (made again on a switch). The page's own strings are pairs tt('русский', 'English').
    const LANGS = [['ru', 'RU'], ['en', 'EN']];
    let lang = 'ru';
    const tt = (ru, en) => (lang === 'en' ? en : ru);
    function localize(x) {
        if (Array.isArray(x)) return x.map(localize);
        if (x && typeof x === 'object' && Object.getPrototypeOf(x) === Object.prototype) {
            const keys = Object.keys(x);
            if (keys.length === 2 && 'ru' in x && 'en' in x) return x[lang];
            const out = {};
            keys.forEach(k => (out[k] = localize(x[k])));
            return out;
        }
        return x;
    }
    let C, byId, secById, PERSONAL;
    function setLang(l) {
        lang = l === 'en' ? 'en' : 'ru';
        C = localize(window.CRYMT);
        C.STATUS = window.CRYMT.STATUS; // filled in by the window, kept across switches
        byId = {};
        C.OPTIONS.forEach(o => (byId[o.id] = o));
        secById = {};
        C.SECTIONS.forEach((sec, i) => (secById[sec.id] = Object.assign({n: String(i + 1).padStart(2, '0')}, sec)));
        PERSONAL = C.OPTIONS.filter(o => o.personal).map(o => o.id); // kept when a preset is picked
        applyMods();
    }
    // The mod select: "no mod" + the folders of <game>\Mods the window found (STATUS.mods).
    function applyMods() {
        if (byId.mod) byId.mod.choices = byId.mod.choices.slice(0, 1).concat((C.STATUS.mods || []).map(m => ({v: m.name, label: m.name})));
    }
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem(STORE) || 'null'); } catch (e) { /* no storage */ }
    const hashLang = (location.hash.match(/lang=(ru|en)/) || [])[1];
    setLang(hashLang || (saved && saved.lang) || (/^ru/i.test(navigator.language || '') ? 'ru' : 'en'));
    const MS_SCALE = 8; // the load meter's full width, ms
    const wv = window.chrome && window.chrome.webview;

    const $ = (s, r) => (r || document).querySelector(s);
    const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
    const esc = s => String(s).replace(/[&<>"]/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[c]));
    function el(tag, cls, html) {
        const e = document.createElement(tag);
        if (cls) e.className = cls;
        if (html != null) e.innerHTML = html;
        return e;
    }
    const fmt = v => String(Math.round(v * 100) / 100);
    const same = I.same;

    // ---------------------------------------------------------------- state
    function defaults() {
        const v = {};
        C.OPTIONS.forEach(o => (v[o.id] = o.def));
        return v;
    }
    const S = {values: defaults(), tab: 'home', expert: false, focus: 'mode', query: '', channel: C.STATUS.channel, iniText: '', busy: false};
    try {
        if (saved) {
            ['tab', 'expert', 'focus', 'channel'].forEach(k => (saved[k] != null ? (S[k] = saved[k]) : 0));
            if (!wv) Object.keys(saved.values || {}).forEach(k => (byId[k] && I.valid(byId[k], saved.values[k]) ? (S.values[k] = saved.values[k]) : 0));
            if (!byId[S.focus]) S.focus = 'mode';
        }
    } catch (e) { /* no storage: defaults */ }
    const hashTab = location.hash.slice(1).split('?')[0];
    const autoAction = (location.hash.match(/auto=(update|rollback)/) || [])[1]; // tests (CryMTLauncher --page=#news?auto=update)
    if (['home', 'adv', 'keys', 'news'].includes(hashTab)) S.tab = hashTab;
    function save() {
        try { localStorage.setItem(STORE, JSON.stringify({values: S.values, tab: S.tab, expert: S.expert, focus: S.focus, channel: S.channel, lang})); } catch (e) { /* ignore */ }
    }

    // ---------------------------------------------------------------- logic
    const MODES = {
        mtMode: v => v.mode !== 'off',
        dx12Mode: v => v.mode === 'dx12' || v.mode === 'dlaa',
        dlaaMode: v => v.mode === 'dlaa',
    };
    const MODE_NEED = () => ({mtMode: tt('режим с CryMT', 'a CryMT mode'), dx12Mode: tt('режим DirectX 12 или DLAA', 'DirectX 12 or DLAA mode'), dlaaMode: tt('режим DLAA', 'DLAA mode')});
    function isOn(o, v) {
        if (o.type === 'toggle') return !!v;
        if (o.type === 'select') return v !== 'off' && v !== o.offValue;
        return true;
    }
    // What has to be turned on for this option to matter (null: it works now). Launcher options always work.
    function blocker(o, vals) {
        if (o.ini[0] === 'Launcher') return null;
        if (o.id !== 'mode' && vals.mode === 'off') return tt('режим с CryMT', 'a CryMT mode');
        if (o.id === 'gi' && MODES.dx12Mode(vals) && modGiOff(vals)) return tt('мод без своих шейдеров GI', 'a mod without its own GI shaders');
        if (!o.needs) return null;
        if (MODES[o.needs]) return MODES[o.needs](vals) ? null : MODE_NEED()[o.needs];
        const p = byId[o.needs];
        return blocker(p, vals) || (isOn(p, vals[p.id]) ? null : p.label);
    }
    // The chosen mod ({name, dll, dll32, shaders, gi} from the window), if its folder is still there.
    const modOf = vals => (vals.mod && (C.STATUS.mods || []).find(m => m.name === vals.mod)) || null;
    // A mod with its own versions of the shaders CryMT's GI patches: the game reads the mod's, so the GI is off for it,
    // unless the player turned on modShaders (GI at their own risk); modGiRisk: that case.
    const modGiOff = vals => { const m = modOf(vals); return !!(m && m.gi && !vals.modShaders); };
    const modGiRisk = vals => { const m = modOf(vals); return !!(m && m.gi && vals.modShaders && MODES.dx12Mode(vals) && vals.gi); };
    const featureOn = (id, vals) => !blocker(byId[id], vals) && isOn(byId[id], vals[id]);
    function loadMs(vals) {
        let ms = 0;
        C.OPTIONS.forEach(o => { if (o.ms && !blocker(o, vals)) ms += o.ms(vals[o.id]); });
        return ms;
    }
    const shadersNeeded = vals => featureOn('gi', vals);
    function launchArgs(vals) {
        const m = modOf(vals);
        return '-dx10' + (vals.devmode ? ' -DEVMODE' : '') + (m ? ' -mod ' + (/\s/.test(m.name) ? '"' + m.name + '"' : m.name) : '') +
            (modGiOff(vals) ? ' -crymt-gi=0' : '');
    }
    // The editor with CryMT (option editorCryMT, a CryMT mode): its GI as the game's, -crymt-gi=0 for a mod with its own shaders.
    const editorCryMT = vals => !!vals.editorCryMT && MODES.mtMode(vals);
    const editorArgs = vals => {
        const m = modOf(vals);
        return (m ? '-mod ' + (/\s/.test(m.name) ? '"' + m.name + '"' : m.name) : '') + (modGiOff(vals) && editorCryMT(vals) ? ' -crymt-gi=0' : '');
    };
    // DLAA on the home page: mode dx12 <-> dlaa, when the mode has DirectX 12 and the card can (NVIDIA RTX).
    const dlaaWhyNot = vals => (!MODES.dx12Mode(vals) ? tt('нужен режим с DirectX 12', 'needs a DirectX 12 mode') :
        C.STATUS.gpu && !C.STATUS.dlss ? tt('нужна NVIDIA RTX, у вас ', 'needs an NVIDIA RTX card, yours: ') + C.STATUS.gpu : null);
    // The game's MSAA (Options, anti-aliasing quality) is turned off by CryMT in every CryMT mode (not supported yet:
    // with it DLAA and GI did not start and shadows were missing). Vanilla keeps it.
    const msaaOff = vals => !!C.STATUS.msaa && MODES.mtMode(vals);
    function valueLabel(o, v) {
        if (o.type === 'toggle') return v ? tt('вкл', 'on') : tt('выкл', 'off');
        if (o.type === 'select') return (o.choices.find(c => c.v === v) || {label: v}).label;
        return fmt(v) + (o.unit ? ' ' + o.unit : '');
    }
    function needChain(o) {
        const out = [];
        for (let n = o.needs; n;) {
            if (MODES[n]) { out.push(MODE_NEED()[n]); break; }
            out.push(byId[n].label);
            n = byId[n].needs;
        }
        return out;
    }
    const iniText = vals => I.merge(C, S.iniText, vals);

    // Presets
    function presetValues(p, base) {
        const v = Object.assign(defaults(), p.values);
        PERSONAL.forEach(k => (v[k] = (base || S.values)[k]));
        return v;
    }
    // {preset, diff}: the preset whose own values match, and how many working options differ from it.
    function currentPreset(vals) {
        for (const p of C.PRESETS) {
            if (!Object.keys(p.values).every(k => same(vals[k], p.values[k]))) continue;
            const pv = presetValues(p, vals);
            const diff = C.OPTIONS.filter(o => !o.personal && !same(vals[o.id], pv[o.id]) && !blocker(o, vals)).length;
            return {preset: p, diff};
        }
        return {preset: null, diff: 0};
    }
    const FEATURES = () => [
        [tt('Многопоточность', 'Multithreading'), v => v.mode !== 'off'],
        ['DirectX 12', v => MODES.dx12Mode(v)],
        ['DLAA', v => MODES.dlaaMode(v)],
        [tt('Глобальное освещение', 'Global illumination'), v => featureOn('gi', v)],
        ['GTAO', v => featureOn('gtao', v)],
        [tt('Контактные тени', 'Contact shadows'), v => featureOn('contact', v)],
        [tt('GI для дыма и пыли', 'GI for smoke and dust'), v => featureOn('particles', v)],
    ];

    // ---------------------------------------------------------------- host
    let saveTimer = 0;
    const send = text => wv && wv.postMessage(text);
    const host = wv ? {
        save: () => { clearTimeout(saveTimer); send('save\n' + iniText(S.values)); },
        saveSoon: () => { clearTimeout(saveTimer); saveTimer = setTimeout(host.save, 400); },
        play: () => {
            host.save();
            S.busy = true;
            syncFooter();
            send('play\n' + (shadersNeeded(S.values) ? '1' : '0') + '\n' + launchArgs(S.values) + '\n' + S.values.afterStart);
        },
        editor: () => {
            host.save();
            S.busy = true;
            syncFooter();
            send('editor\n' + (editorCryMT(S.values) && shadersNeeded(S.values) ? '1' : '0') + '\n' + (editorCryMT(S.values) ? '1' : '0') + '\n' +
                 editorArgs(S.values) + '\n' + S.values.afterStart);
        },
        drag: () => send('drag'),
        min: () => send('min'),
        max: () => send('max'),
        resize: edge => send('resize\n' + edge),
        close: () => { host.save(); send('close'); },
        open: what => send('open\n' + what),
        fetch: (id, url) => send('fetch\n' + id + '\n' + url),
        manifest: (url, sig) => send('update-manifest\n' + url + '\n' + sig),
        install: zip => send('update-install\n' + zip),
        rollback: () => send('rollback'),
        verify: () => send('verify'),
        restart: () => send('restart'),
    } : {
        save: () => {},
        saveSoon: () => {},
        play: () => toast(tt('В окне лаунчера: записать CryMT.ini → ', 'In the launcher window: write CryMT.ini → ') + (shadersNeeded(S.values) ? tt('поставить шейдеры GI → ', 'install the GI shaders → ') : tt('убрать шейдеры GI → ', 'remove the GI shaders → ')) +
            tt('запустить Bin64\\Crysis.exe ', 'start Bin64\\Crysis.exe ') + launchArgs(S.values)),
        editor: () => toast(tt('В окне лаунчера: запустить редактор ', 'In the launcher window: start the editor ') + (editorCryMT(S.values) ? tt('с CryMT ', 'with CryMT ') : '') + editorArgs(S.values)),
        drag: () => {},
        max: () => {},
        resize: () => {},
        min: () => toast(tt('Свернуть — в окне лаунчера', 'Minimize — in the launcher window')),
        close: () => toast(tt('Закрыть — в окне лаунчера', 'Close — in the launcher window')),
        open: what => toast(tt('Открыть папку «', 'Open folder «') + what + tt('» — в окне лаунчера', '» — in the launcher window')),
        // In a plain browser the release list still comes from GitHub (its API allows pages of any origin).
        fetch: (id, url) => window.fetch(url).then(r => r.text().then(body => onHost({type: 'fetched', id, ok: r.ok, status: r.status, body})),
                                                   e => onHost({type: 'fetched', id, ok: false, status: 0, error: String(e), body: ''})),
        manifest: () => onHost({type: 'manifest', ok: false, error: tt('подпись проверяет окно лаунчера', 'the launcher window checks the signature')}),
        install: () => toast(tt('Обновление ставит окно лаунчера', 'the launcher window installs updates')),
        rollback: () => toast(tt('Откат — в окне лаунчера', 'Rollback — in the launcher window')),
        verify: () => toast(tt('Проверка файлов — в окне лаунчера', 'File check — in the launcher window')),
        restart: () => toast(tt('Перезапуск — в окне лаунчера', 'Restart — in the launcher window')),
    };

    // Updates: releases of C.UPDATE.repo on GitHub. Stable channel: releases; test channel: pre-releases too. A release
    // newer than the installed CryMT\version.txt is offered after the window has checked its manifest's signature.
    S.upd = {state: 'idle', releases: null, release: null, manifest: null, progress: null, error: ''};
    const tagNumber = tag => parseInt(String(tag || '').replace(/[^0-9]/g, ''), 10) || 0;
    const asset = (rel, test) => (rel.assets || []).find(a => test(a.name));
    function checkUpdates() {
        S.upd = Object.assign(S.upd, {state: 'checking', error: '', release: null, manifest: null, progress: null});
        buildNews();
        syncFooter();
        host.fetch('releases', C.UPDATE.api);
    }
    function pickRelease() {
        const list = (S.upd.releases || []).filter(r => !r.draft && (S.channel === 'test' || !r.prerelease));
        list.sort((x, y) => tagNumber(y.tag_name) - tagNumber(x.tag_name));
        const rel = list.find(r => asset(r, n => n === 'manifest.txt') && asset(r, n => n === 'manifest.sig') && asset(r, n => /\.zip$/i.test(n)));
        const installed = tagNumber(C.STATUS.installed);
        if (!rel) return Object.assign(S.upd, {state: 'none'});
        S.upd.release = rel;
        if (installed && tagNumber(rel.tag_name) <= installed) return Object.assign(S.upd, {state: 'latest'});
        S.upd.state = 'verifying';
        host.manifest(asset(rel, n => n === 'manifest.txt').browser_download_url, asset(rel, n => n === 'manifest.sig').browser_download_url);
    }
    function installUpdate() {
        const u = S.upd;
        if (u.state !== 'available') return;
        showTab('news');
        Object.assign(u, {state: 'installing', progress: null});
        buildNews();
        syncFooter();
        host.install(asset(u.release, n => /\.zip$/i.test(n)).browser_download_url);
    }
    function restartLauncher() {
        host.save();
        Object.assign(S.upd, {state: 'restarting'});
        buildNews();
        syncFooter();
        host.restart();
    }
    function parseManifest(text) {
        const lines = String(text || '').split(/\r?\n/);
        const head = (lines[0] || '').split(' ');
        const date = (lines.find(l => l.startsWith('date ')) || '').slice(5);
        const files = lines.filter(l => /^(file|keep) /.test(l)).map(l => l.split(' '));
        return {version: head[1] || '?', date, files: files.length, bytes: files.reduce((n, f) => n + (parseInt(f[2], 10) || 0), 0)};
    }
    if (wv) {
        wv.addEventListener('message', e => onHost(typeof e.data === 'string' ? JSON.parse(e.data) : e.data));
        send('load');
    }
    let firstState = true;
    function onHost(m) {
        if (m.type === 'state') {
            S.iniText = m.ini || '';
            Object.assign(C.STATUS, m.status || {});
            applyMods(); // before reading: a Mod= line is valid only for a folder that is there
            if (m.exists) S.values = I.readValues(C, S.iniText);
            document.documentElement.classList.toggle('maxed', !!C.STATUS.max);
            if (m.reason === 'external') toast(tt('CryMT.ini изменён вне лаунчера — настройки перечитаны', 'CryMT.ini changed outside the launcher — settings read again'));
            if (firstState) {
                firstState = false;
                if (autoAction === 'rollback') host.rollback();
                else checkUpdates();
            }
            buildNews();
            sync();
        } else if (m.type === 'saved') {
            S.iniText = m.ini;
        } else if (m.type === 'played') {
            S.busy = false;
            syncFooter();
            toast(m.ok ? (m.editor ? tt('Редактор запущен', 'Editor started') : tt('Игра запущена', 'Game started')) : tt('Не удалось запустить: ', 'Could not start: ') + m.error);
        } else if (m.type === 'window') {
            document.documentElement.classList.toggle('maxed', !!m.max);
            $('#winMax').title = m.max ? tt('Восстановить', 'Restore') : tt('Развернуть', 'Maximize');
        } else if (m.type === 'fetched' && m.id === 'releases') {
            if (!m.ok) Object.assign(S.upd, {state: 'error', error: tt('GitHub не ответил: ', 'GitHub did not answer: ') + (m.error || m.status)});
            else {
                try {
                    S.upd.releases = JSON.parse(m.body);
                    pickRelease();
                } catch (e) {
                    Object.assign(S.upd, {state: 'error', error: tt('ответ GitHub не читается', 'GitHub\'s answer cannot be read')});
                }
            }
            buildNews();
            syncFooter();
        } else if (m.type === 'manifest') {
            if (m.ok) {
                Object.assign(S.upd, {state: 'available', manifest: parseManifest(m.text)});
                if (autoAction === 'update') setTimeout(installUpdate, 500);
                else if (S.tab !== 'news')
                    toast(tt('Доступна CryMT v', 'CryMT v') + S.upd.manifest.version + tt('', ' is available'), {label: tt('Обновить', 'Update'), run: installUpdate});
            } else {
                Object.assign(S.upd, {state: 'error', error: m.error || tt('проверка не удалась', 'the check failed')});
            }
            buildNews();
            syncFooter();
        } else if (m.type === 'progress') {
            S.upd.progress = m;
            buildNews();
        } else if (m.type === 'installed' || m.type === 'rolledback') {
            const what = m.type === 'installed' ? tt('Обновление до v', 'Update to v') + m.version : tt('Откат', 'Rollback');
            if (m.ok) {
                // Files of the launcher itself changed: the window and this page are the old ones until a restart.
                Object.assign(S.upd, {state: 'done', progress: null, done: what, restart: !!m.restart});
                if (m.restart && autoAction) setTimeout(restartLauncher, 1500); // tests (--page=#news?auto=...)
                else if (m.restart) toast(what + tt(': готово. Перезапустите лаунчер', ': done. Restart the launcher'), {label: tt('Перезапустить', 'Restart'), run: restartLauncher});
                else toast(what + tt(': готово', ': done'));
                send('load');
            } else {
                Object.assign(S.upd, {state: 'error', error: m.error, progress: null});
                toast(what + ': ' + m.error);
            }
            buildNews();
            syncFooter();
        } else if (m.type === 'verified') {
            toast(!m.ok ? tt('Нет CryMT\\version.txt: файлы ставились не через лаунчер', 'No CryMT\\version.txt: the files were not installed by the launcher') :
                m.wrong ? tt('Проверено файлов: ', 'Files checked: ') + m.checked + tt(', не совпало: ', ', differing: ') + m.wrong + ' (' + m.bad.join(', ') + ')' :
                tt('Проверено файлов: ', 'Files checked: ') + m.checked + tt(', все совпали с версией v', ', all equal to version v') + C.STATUS.installed);
        } else if (m.type === 'error') {
            S.busy = false;
            syncFooter();
            toast(m.error);
        }
    }

    // ---------------------------------------------------------------- common widgets
    function toast(msg, action) {
        const t = el('div', 'toast panel cut', '<span>' + esc(msg) + '</span>');
        if (action) {
            const b = el('button', 'btn small', esc(action.label));
            b.addEventListener('click', () => { action.run(); t.remove(); });
            t.append(b);
        }
        $('#toasts').append(t);
        setTimeout(() => t.classList.add('out'), action ? 7000 : 4200);
        setTimeout(() => t.remove(), action ? 7600 : 4800);
    }
    function changed() {
        save();
        host.saveSoon();
        sync();
    }
    function setValues(next, msg) {
        const prev = Object.assign({}, S.values);
        S.values = next;
        changed();
        if (msg) toast(msg, {label: tt('Отменить', 'Undo'), run: () => { S.values = prev; changed(); }});
    }
    function set(id, v) {
        S.values[id] = v;
        S.focus = id;
        changed();
    }
    function seg(items, cur, onPick) {
        const box = el('div', 'seg');
        items.forEach(([v, label, title]) => {
            const b = el('button', v === cur ? 'on' : '', esc(label));
            b.type = 'button';
            b.dataset.v = v;
            if (title) b.title = title;
            b.addEventListener('click', () => onPick(v));
            box.append(b);
        });
        return box;
    }
    function toggleButton(on, onClick) {
        const b = el('button', 'tg', tt('<span>Выкл</span><span>Вкл</span>', '<span>Off</span><span>On</span>'));
        b.type = 'button';
        b.setAttribute('aria-pressed', on ? 'true' : 'false');
        b.addEventListener('click', onClick);
        return b;
    }
    const pct = (v, o) => Math.max(0, Math.min(100, ((v - o.min) / (o.max - o.min)) * 100));
    function control(o) {
        const wrap = el('div', 'ctl');
        if (o.type === 'toggle') {
            wrap.append(toggleButton(S.values[o.id], () => set(o.id, !S.values[o.id])));
        } else if (o.type === 'select' && o.dropdown) {
            const box = el('select', 'ctl-select');
            box.setAttribute('aria-label', o.label);
            box.addEventListener('change', () => set(o.id, box.value));
            wrap.append(box);
        } else if (o.type === 'select') {
            wrap.append(seg(o.choices.map(c => [c.v, c.label, c.desc]), S.values[o.id], v => set(o.id, v)));
        } else {
            const box = el('div', 'sl');
            const track = el('div', 'sl-track');
            const r = el('input');
            r.type = 'range';
            r.min = o.min; r.max = o.max; r.step = o.step; r.value = S.values[o.id];
            r.setAttribute('aria-label', o.label);
            r.addEventListener('input', () => set(o.id, parseFloat(r.value)));
            const d = el('i', 'sl-def');
            const p = pct(o.def, o);
            d.style.left = 'calc(' + p + '% + ' + (0.5 - p / 100) * 12 + 'px)';
            d.title = tt('по умолчанию: ', 'default: ') + valueLabel(o, o.def);
            track.append(r, d);
            const val = el('button', 'sl-v');
            val.type = 'button';
            val.title = tt('Двойной щелчок — значение по умолчанию', 'Double-click: default value');
            val.addEventListener('dblclick', () => set(o.id, o.def));
            box.append(track, val);
            wrap.append(box);
        }
        return wrap;
    }
    function syncControl(wrap, o) {
        const v = S.values[o.id];
        if (o.type === 'toggle') $('.tg', wrap).setAttribute('aria-pressed', v ? 'true' : 'false');
        else if (o.type === 'select' && o.dropdown) {
            const box = $('select', wrap);
            // The choices can change (the window rescans Mods): the list is made again when they differ.
            if (Array.from(box.options).map(x => x.value).join('\n') !== o.choices.map(x => x.v).join('\n') ||
                Array.from(box.options).map(x => x.textContent).join('\n') !== o.choices.map(x => x.label).join('\n')) {
                box.textContent = '';
                o.choices.forEach(x => { const opt = el('option'); opt.value = x.v; opt.textContent = x.label; box.append(opt); });
            }
            box.value = v;
        } else if (o.type === 'select') $$('.seg button', wrap).forEach(b => b.classList.toggle('on', b.dataset.v === v));
        else {
            const r = $('input', wrap);
            if (document.activeElement !== r) r.value = v;
            r.style.setProperty('--p', pct(v, o) + '%');
            const val = $('.sl-v', wrap);
            val.textContent = valueLabel(o, v);
            val.classList.toggle('chg', !same(v, o.def));
        }
    }
    function badges(o) {
        let h = '';
        if (o.hotkey) h += tt('<kbd title="Клавиша в игре">', '<kbd title="Key in game">') + esc(o.hotkey) + '</kbd>';
        if (o.cost) h += tt('<span class="chip cost" title="Цена на видеокарте">', '<span class="chip cost" title="GPU cost">') + esc(o.cost) + '</span>';
        if (o.shaders) h += tt('<span class="chip shd" title="Нужны шейдеры CryMT">шейдеры</span>', '<span class="chip shd" title="Needs CryMT shaders">shaders</span>');
        if (o.expert) h += tt('<span class="chip exp" title="Для энтузиастов">тонкая</span>', '<span class="chip exp" title="For enthusiasts">fine-tuning</span>');
        return h;
    }
    const meter = ms => '<span class="meter"><i style="width:' + Math.min(100, (ms / MS_SCALE) * 100) + '%"></i></span>';

    // ---------------------------------------------------------------- title bar and tabs
    const TABS = () => [['home', tt('Главная', 'Home')], ['adv', tt('Настройки', 'Settings')], ['keys', tt('Клавиши', 'Keys')], ['news', tt('Обновления', 'Updates')]];
    function buildTabs() {
        const tabs = $('#tabs');
        tabs.innerHTML = '';
        TABS().forEach(([id, label]) => {
            const b = el('button', 'tab', '<span>' + esc(label) + '</span>');
            b.type = 'button';
            b.dataset.tab = id;
            b.setAttribute('role', 'tab');
            b.addEventListener('click', () => showTab(id));
            tabs.append(b);
        });
    }
    function buildHeader() {
        buildTabs();
        const langSeg = seg(LANGS, lang, l => switchLang(l));
        langSeg.id = 'langSeg';
        $('#langSeg').replaceWith(langSeg);
        // The header is the window's title bar: dragging its empty parts moves the window.
        $('.top').addEventListener('mousedown', e => {
            if (e.button === 0 && !e.target.closest('button, input, a, [role="button"]')) { e.preventDefault(); host.drag(); }
        });
        $('.top').addEventListener('dblclick', e => { if (!e.target.closest('button, input, a, select, [role="button"]')) host.max(); });
        $$('.grips i').forEach(g => g.addEventListener('mousedown', e => { if (e.button === 0) { e.preventDefault(); host.resize(g.dataset.e); } }));
        $('#winMax').addEventListener('click', host.max);
        $('#winMin').addEventListener('click', host.min);
        $('#winClose').addEventListener('click', host.close);
        $('#play').addEventListener('click', () => { if (!S.busy) host.play(); });
        document.documentElement.classList.toggle('in-host', !!wv);
    }
    // A language switch: every view is drawn again in the other language; the window gets it for its own messages.
    function switchLang(l) {
        if (l === lang) return;
        setLang(l);
        save();
        applyLangChrome();
        buildTabs();
        buildHome();
        buildAdv();
        buildKeys();
        buildNews();
        showTab(S.tab);
    }
    function applyLangChrome() {
        document.documentElement.lang = lang;
        $$('#langSeg button').forEach(b => b.classList.toggle('on', b.dataset.v === lang));
        $('.brand-sub').textContent = tt('MT · DirectX 12 · DLAA · GI для Crysis 1.2.1', 'MT · DirectX 12 · DLAA · GI for Crysis 1.2.1');
        $('#winMin').title = tt('Свернуть', 'Minimize');
        $('#winMax').title = document.documentElement.classList.contains('maxed') ? tt('Восстановить', 'Restore') : tt('Развернуть', 'Maximize');
        $('#winClose').title = tt('Закрыть', 'Close');
        send('lang\n' + lang);
    }
    function showTab(id) {
        S.tab = id;
        save();
        $$('.tab').forEach(b => b.classList.toggle('on', b.dataset.tab === id));
        $$('.view').forEach(v => v.classList.toggle('on', v.id === 'view-' + id));
        sync();
    }

    // ---------------------------------------------------------------- home (presets)
    // The author's FPS for a preset on each test (BENCH in options.js): average, + the change against vanilla.
    function benchBlock(p) {
        const mine = C.BENCH.fps[p.id], base = C.BENCH.fps.vanilla;
        if (!mine) return '';
        return '<div class="bench">' + C.BENCH.tests.map(t => {
            const v = mine[t.id], b = base && base[t.id];
            if (!v) return '<div title="' + esc(t.hint) + '"><span>' + esc(t.label) + '</span><b>—</b></div>';
            const gain = b && p.id !== 'vanilla' ? Math.round((v[0] / b[0] - 1) * 100) : null;
            return '<div title="' + esc(t.hint + ' · ' + tt('мин.', 'min') + ' ' + fmt(v[1])) + '"><span>' + esc(t.label) + '</span><b>' + Math.round(v[0]) +
                (gain === null ? '' : '<i class="' + (gain >= 0 ? 'up' : 'down') + '">' + (gain >= 0 ? '+' : '') + gain + '%</i>') + '</b></div>';
        }).join('') + '</div>';
    }

    function buildHome() {
        const v = $('#view-home');
        v.innerHTML = tt('<div class="home-head"><h2>Режим запуска</h2><p>Выберите готовый набор. Всё тонкое — во вкладке «Настройки», ', '<div class="home-head"><h2>Launch mode</h2><p>Pick a ready-made set. Everything finer is on the Settings tab, ') +
            tt('там же подсказки, что за что отвечает.</p></div>', 'with explanations of what each option does.</p></div>');
        const grid = el('div', 'presets');
        C.PRESETS.forEach((p, i) => {
            const pv = presetValues(p);
            const ms = loadMs(pv);
            const card = el('div', 'card panel cut');
            card.dataset.p = p.id;
            card.tabIndex = 0;
            card.setAttribute('role', 'button');
            card.innerHTML =
                '<div class="card-top"><span class="card-idx">' + String(i + 1).padStart(2, '0') + '</span><span class="card-tag">' + esc(p.tag) + '</span></div>' +
                '<div class="card-name">' + esc(p.label) + '</div>' +
                '<div class="card-text">' + esc(p.text) + '</div>' +
                '<ul class="feats">' + FEATURES().map(([n, f]) => '<li class="' + (f(pv) ? 'on' : '') + '"><i></i>' + esc(n) + '</li>').join('') + '</ul>' +
                tt('<div class="card-meta card-gpu"><span>Видеокарта</span><b>', '<div class="card-meta card-gpu"><span>GPU</span><b>') + (ms ? '≈ ' + fmt(ms) + tt(' мс', ' ms') : '—') + '</b></div>' + meter(ms) +
                tt('<div class="card-meta"><span>Шейдеры</span><b>', '<div class="card-meta"><span>Shaders</span><b>') + esc(p.shaders) + '</b></div>' +
                benchBlock(p) +
                '<div class="card-badge"></div>';
            const pick = () => {
                const cur = currentPreset(S.values);
                if (cur.preset === p && cur.diff === 0) return;
                const custom = cur.diff > 0 || !cur.preset;
                setValues(presetValues(p), custom ? tt('Режим «', 'Mode «') + p.label + tt('»: свои настройки сброшены', '»: your own settings were reset') : null);
            };
            card.addEventListener('click', pick);
            card.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } });
            // The Full preset's quality switches (GI, reflections): on the card itself; a click on them does not pick the
            // card (they apply to it whenever it is picked: personal options).
            if (p.id === 'full') {
                const q = el('div', 'card-q');
                [byId.giQuality, byId.reflQuality].filter(Boolean).forEach(o => {
                    const row = el('div', 'card-q-row');
                    row.dataset.o = o.id;
                    row.append(el('span', 'card-q-label', esc(o.label)), control(o));
                    q.append(row);
                });
                ['click', 'keydown'].forEach(ev => q.addEventListener(ev, e => e.stopPropagation()));
                card.insertBefore(q, $('.card-badge', card));
            }
            grid.append(card);
        });
        v.append(grid);
        v.append(el('div', 'bench-note', esc(tt('FPS — замер автора, второй из двух проходов: ', 'FPS: the author\'s measurement, second of two passes: ') + C.BENCH.system +
            tt('. У вас будет по-другому.', '. Yours will differ.'))));
        const low = el('div', 'home-low');
        low.append(el('div', 'panel cut detail'));
        const quick = el('div', 'panel cut quick');
        quick.append(el('h3', null, tt('Запуск и окно', 'Launch and window')));
        const dlaa = el('div', 'opt compact lead dlaa-row');
        dlaa.innerHTML = tt('<div class="opt-main"><div class="opt-label">Сглаживание DLAA</div><div class="opt-why"></div></div>',
                            '<div class="opt-main"><div class="opt-label">DLAA anti-aliasing</div><div class="opt-why"></div></div>');
        const dlaaCtl = el('div', 'ctl');
        dlaaCtl.append(toggleButton(false, () => { if (!dlaaWhyNot(S.values)) set('mode', S.values.mode === 'dlaa' ? 'dx12' : 'dlaa'); }));
        dlaa.append(dlaaCtl);
        quick.append(dlaa, optRow(byId.mod, true));
        PERSONAL.filter(id => id !== 'mod' && !byId[id].expert).forEach(id => quick.append(optRow(byId[id], true)));
        const btns = el('div', 'quick-btns');
        const adv = el('button', 'btn ghost', tt('Тонкая настройка', 'Fine-tuning'));
        adv.addEventListener('click', () => showTab('adv'));
        const mods = el('button', 'btn ghost', tt('Папка Mods', 'Mods folder'));
        mods.addEventListener('click', () => host.open('mods'));
        // Shown only when the window finds Bin64\Editor.exe (syncHome).
        const ed = el('button', 'btn ghost editor', tt('Редактор', 'Editor'));
        ed.addEventListener('click', () => { if (!S.busy) host.editor(); });
        btns.append(adv, mods, ed);
        quick.append(btns);
        low.append(quick);
        v.append(low);
    }
    function syncHome() {
        const cur = currentPreset(S.values);
        $$('#view-home .card').forEach(c => {
            const mine = cur.preset && cur.preset.id === c.dataset.p;
            c.classList.toggle('on', !!mine);
            $('.card-badge', c).innerHTML = mine ? (cur.diff ? tt('выбрано · изменено: ', 'selected · changed: ') + cur.diff : tt('выбрано', 'selected')) : '';
            // The quality switches change the card's GPU load: its figure and meter follow them.
            const p = C.PRESETS.find(x => x.id === c.dataset.p);
            const gpu = $('.card-gpu b', c);
            if (p && gpu) {
                const ms = loadMs(presetValues(p));
                gpu.textContent = ms ? '≈ ' + fmt(ms) + tt(' мс', ' ms') : '—';
                const bar = $('.meter i', c);
                if (bar) bar.style.width = Math.min(100, (ms / MS_SCALE) * 100) + '%';
            }
        });
        $$('#view-home .card-q-row').forEach(r => syncControl($('.ctl', r), byId[r.dataset.o]));
        const vals = S.values;
        const rows = C.OPTIONS.filter(o => o.ms && !blocker(o, vals) && o.ms(vals[o.id]) > 0);
        const ms = loadMs(vals);
        const notes = [];
        if (vals.mode === 'off') notes.push(tt('CryMT не загружается: игра запускается как есть, со своими настройками.', 'CryMT is not loaded: the game starts as it is, with its own settings.'));
        if (MODES.mtMode(vals)) notes.push(tt('Многопоточность ускоряет процессорную часть кадра; на видеокарту не влияет.', 'Multithreading speeds up the CPU part of the frame; it does not change the GPU load.'));
        if (MODES.dlaaMode(vals))
            notes.push(C.STATUS.gpu && !C.STATUS.dlss
                ? tt('Видеокарта ', 'The graphics card ') + C.STATUS.gpu + tt(' не поддерживает DLAA (нужна NVIDIA RTX): игра сохранит своё сглаживание, остальное работает.', ' has no DLAA (it needs an NVIDIA RTX): the game keeps its own anti-aliasing, the rest works.')
                : tt('DLAA работает только на видеокартах NVIDIA RTX.', 'DLAA works only on NVIDIA RTX cards.'));
        const mod = modOf(vals);
        if (mod) {
            notes.push(tt('Мод «', 'Mod «') + mod.name + tt('»: игра запускается с -mod ', '»: the game starts with -mod ') + mod.name +
                (mod.dll ? tt('. У мода своя DLL игры — CryMT с ней работает.', '. It has its own game DLL; CryMT works with it.') : '.'));
            if (modGiRisk(vals))
                notes.push(tt('Мод меняет шейдеры, которые правит GI, а GI включён на ваш риск («GI с модом, у которого свои шейдеры GI»): GI виден только там, где мод не заменил шейдеры, возможны ошибки картинки.', 'The mod changes the shaders the GI patches and the GI is on at your own risk ("GI with a mod that has its own GI shaders"): the GI shows only where the mod kept the shaders, the picture can break.'));
            else if (mod.gi && MODES.dx12Mode(vals) && vals.gi)
                notes.push(tt('Мод меняет шейдеры, которые правит GI: с ним GI выключается (-crymt-gi=0), остальное работает. Запустить с GI на свой риск: «GI с модом, у которого свои шейдеры GI».', 'The mod changes the shaders the GI patches: with it the GI is off (-crymt-gi=0), the rest works. To start it with the GI at your own risk: "GI with a mod that has its own GI shaders".'));
            else if (mod.shaders && featureOn('gi', vals))
                notes.push(tt('Мод меняет шейдеры (не те, что правит GI). Если картинка сломается — «MT + DLAA».', 'The mod changes shaders (not the ones the GI patches). If the image breaks, use «MT + DLAA».'));
            if (mod.dll32) notes.push(tt('У мода только 32-битная DLL (Bin32): в 64-битной игре его код не загрузится.', 'The mod has only a 32-bit DLL (Bin32): its code does not load in the 64-bit game.'));
        } else if (vals.mod) notes.push(tt('Мода «', 'The mod «') + vals.mod + tt('» нет в папке Mods — запуск без мода.', '» is not in the Mods folder: starting without a mod.'));
        if (msaaOff(vals))
            notes.push(tt('В настройках игры включено сглаживание MSAA', 'The game\'s MSAA anti-aliasing is on') + ' (' + C.STATUS.msaa + '). ' +
                       tt('С CryMT оно пока не поддерживается и выключается при запуске. В режиме «Ваниль» работает как обычно.',
                          'CryMT does not support it yet and turns it off at start. In Vanilla mode it works as usual.'));
        if (shadersNeeded(vals)) notes.push(tt('GI ставит 5 файлов шейдеров (Game\\zzCryMT.pak). Со сторонними шейдерами GI не совместим — для них «Только MT» или «MT + DLAA».', 'GI installs 5 shader files (Game\\zzCryMT.pak). It does not work with third-party shaders — use «MT only» or «MT + DLAA» with them.'));
        $('#view-home .detail').innerHTML =
            tt('<h3>Сейчас: ', '<h3>Now: ') + esc(cur.preset ? cur.preset.label : tt('свои настройки', 'own settings')) + (cur.diff ? tt(' <span class="chip">изменено: ', ' <span class="chip">changed: ') + cur.diff + '</span>' : '') + '</h3>' +
            '<div class="cost-list">' + rows.map(o => '<div><span>' + esc(o.label) + (o.type === 'select' ? ' · ' + esc(valueLabel(o, vals[o.id])) : '') +
                '</span><b>' + fmt(o.ms(vals[o.id])) + tt(' мс</b></div>', ' ms</b></div>')).join('') +
            tt('<div class="total"><span>Итого на видеокарте</span><b>', '<div class="total"><span>Total on the GPU</span><b>') + (ms ? '≈ ' + fmt(ms) + tt(' мс', ' ms') : '0') + '</b></div></div>' +
            tt('<p class="fine">Оценка по замерам автора, 2560×1440: разница кадра между пресетами. При 60 FPS кадр — 16.7 мс.</p>', '<p class="fine">Estimate from the author\'s measurements, 2560×1440: frame time differences between presets. At 60 FPS a frame is 16.7 ms.</p>') +
            '<ul class="notes">' + notes.map(n => '<li>' + esc(n) + '</li>').join('') + '</ul>';
        $$('#view-home .quick .opt[data-id]').forEach(syncRow);
        const edBtn = $('#view-home .quick .editor');
        edBtn.hidden = !C.STATUS.editor;
        edBtn.title = editorCryMT(vals)
            ? tt('Sandbox Editor из Mod SDK с выбранным модом и CryMT (DirectX 12, GI; без DLAA). Без CryMT: выключите «Редактор с CryMT».',
                 'The Mod SDK\'s Sandbox Editor with the chosen mod and CryMT (DirectX 12, GI; no DLAA). Without CryMT: turn off "Editor with CryMT".')
            : tt('Sandbox Editor из Mod SDK с выбранным модом, без CryMT', 'The Mod SDK\'s Sandbox Editor with the chosen mod, without CryMT');
        const dlaa = $('#view-home .dlaa-row');
        const why = dlaaWhyNot(vals);
        dlaa.classList.toggle('off', !!why);
        $('.opt-why', dlaa).textContent = why || '';
        $('.tg', dlaa).setAttribute('aria-pressed', !why && vals.mode === 'dlaa' ? 'true' : 'false');
    }

    // ---------------------------------------------------------------- advanced
    function optRow(o, compact) {
        const row = el('div', 'opt' + (compact ? ' compact' : ''));
        row.dataset.id = o.id;
        row.innerHTML = '<div class="opt-main"><div class="opt-label">' + esc(o.label) + (compact || !S.expert ? '' : '<span class="badges">' + badges(o) + '</span>') +
            '</div><div class="opt-why"></div></div>';
        row.append(control(o));
        row.addEventListener('mouseenter', () => focusOpt(o.id));
        row.addEventListener('focusin', () => focusOpt(o.id));
        return row;
    }
    function syncRow(row) {
        const o = byId[row.dataset.id];
        const b = blocker(o, S.values);
        row.classList.toggle('off', !!b);
        row.classList.toggle('cur', S.focus === o.id && !row.classList.contains('compact'));
        $('.opt-why', row).textContent = b ? tt('не действует: нужно — ', 'no effect: needs ') + b : '';
        syncControl($('.ctl', row), o);
    }
    function focusOpt(id) {
        if (S.focus === id) return;
        S.focus = id;
        syncAdvSide();
        $$('#view-adv .opt').forEach(r => r.classList.toggle('cur', r.dataset.id === id));
    }
    function matches(o, q) {
        if (!q) return true;
        const text = [o.label, o.hint, o.affects, o.hotkey, secById[o.section].label].concat(I.keysOf(o).map(k => k[1])).join(' ').toLowerCase();
        return q.toLowerCase().split(/\s+/).filter(Boolean).every(t => text.includes(t));
    }
    function buildAdv() {
        const v = $('#view-adv');
        v.innerHTML = '';
        // Without "for enthusiasts": no CryMT.ini panel and no badges in the rows; the help has the whole side.
        const wrap = el('div', 'adv' + (S.expert ? '' : ' simple'));
        const nav = el('aside', 'nav panel cut');
        nav.append(el('div', 'nav-title', tt('Разделы', 'Sections')));
        C.SECTIONS.forEach(s => {
            const a = el('button', 'nav-item', '<span class="sec-n">' + secById[s.id].n + '</span><span>' + esc(s.label) + '</span><em></em>');
            a.type = 'button';
            a.dataset.sec = s.id;
            a.addEventListener('click', () => {
                const sec = $('#sec-' + s.id);
                if (sec) $('.list-body').scrollTo({top: sec.offsetTop - 6, behavior: 'smooth'});
            });
            nav.append(a);
        });
        const exp = el('div', 'nav-exp');
        exp.append(el('div', 'nav-exp-text', tt('<b>Для энтузиастов</b><span>все параметры, включая отладочные</span>', '<b>For enthusiasts</b><span>all settings, debugging ones too</span>')));
        exp.append(toggleButton(S.expert, () => { S.expert = !S.expert; save(); buildAdv(); sync(); }));
        nav.append(exp);
        const reset = el('button', 'btn ghost', tt('Всё по умолчанию', 'All to defaults'));
        reset.addEventListener('click', () => setValues(presetValues(C.PRESETS[C.PRESETS.length - 1]), tt('Все настройки — по умолчанию', 'All settings at their defaults')));
        nav.append(reset);

        const list = el('div', 'list panel cut');
        const head = el('div', 'list-head');
        const search = el('input', 'search');
        search.type = 'search';
        search.placeholder = tt('Поиск: опция, ключ ini, клавиша, «трава», «дым»…', 'Search: option, ini key, key, «grass», «smoke»…');
        search.value = S.query;
        search.addEventListener('input', () => { S.query = search.value.trim(); buildList(); sync(); });
        head.append(search);
        list.append(head, el('div', 'list-body'));
        $('.list-body', list).addEventListener('scroll', syncNav);

        const side = el('aside', 'side');
        side.append(el('div', 'help panel cut'));
        const ini = el('div', 'ini panel cut');
        ini.innerHTML = tt('<div class="ini-head"><span>CryMT.ini</span><span class="ini-btns"><button class="btn small ghost" type="button" data-a="copy">Копировать</button>', '<div class="ini-head"><span>CryMT.ini</span><span class="ini-btns"><button class="btn small ghost" type="button" data-a="copy">Copy</button>') +
            tt('<button class="btn small ghost" type="button" data-a="folder">Папка</button></span></div><pre></pre>', '<button class="btn small ghost" type="button" data-a="folder">Folder</button></span></div><pre></pre>');
        $('[data-a="copy"]', ini).addEventListener('click', () => {
            const text = iniText(S.values);
            const done = () => toast(tt('CryMT.ini скопирован', 'CryMT.ini copied'));
            if (navigator.clipboard) navigator.clipboard.writeText(text).then(done, () => toast(tt('Буфер обмена недоступен', 'Clipboard not available')));
            else toast(tt('Буфер обмена недоступен', 'Clipboard not available'));
        });
        $('[data-a="folder"]', ini).addEventListener('click', () => host.open('crymt'));
        if (S.expert) side.append(ini);
        wrap.append(nav, list, side);
        v.append(wrap);
        buildList();
    }
    function buildList() {
        const body = $('#view-adv .list-body');
        body.innerHTML = '';
        const q = S.query;
        let shown = 0;
        C.SECTIONS.forEach(s => {
            const all = C.OPTIONS.filter(o => o.section === s.id && matches(o, q));
            const vis = all.filter(o => q || S.expert || !o.expert);
            const hidden = all.length - vis.length;
            const navItem = $('#view-adv .nav-item[data-sec="' + s.id + '"]');
            navItem.classList.toggle('empty', vis.length === 0);
            $('em', navItem).textContent = vis.length ? vis.length : '';
            if (!vis.length) return;
            shown += vis.length;
            const sec = el('div', 'sec');
            sec.id = 'sec-' + s.id;
            sec.innerHTML = '<h3><span class="sec-n">' + secById[s.id].n + '</span>' + esc(s.label) +
                (hidden ? '<button class="sec-more" type="button">+' + hidden + tt(' для энтузиастов</button>', ' for enthusiasts</button>') : '') + '</h3>';
            if (hidden) $('.sec-more', sec).addEventListener('click', () => { S.expert = true; save(); buildAdv(); sync(); });
            vis.forEach(o => sec.append(optRow(o)));
            body.append(sec);
        });
        if (!shown) body.innerHTML = tt('<div class="empty-list">Ничего не нашлось по «', '<div class="empty-list">Nothing found for «') + esc(q) + '»</div>';
    }
    function syncNav() {
        const body = $('#view-adv .list-body');
        let cur = null;
        $$('.sec', body).forEach(s => { if (s.offsetTop - 40 <= body.scrollTop) cur = s.id.slice(4); });
        if (!cur) { const first = $('.sec', body); cur = first ? first.id.slice(4) : null; }
        $$('#view-adv .nav-item').forEach(a => a.classList.toggle('on', a.dataset.sec === cur));
    }
    function syncAdvSide() {
        const o = byId[S.focus] || byId.mode;
        const v = S.values[o.id];
        const b = blocker(o, S.values);
        const chain = needChain(o);
        const vals = I.writeOf(o, v);
        const iniDesc = I.keysOf(o).map(([sec, key], i) => (vals[i] != null ? '<code>[' + esc(sec) + '] ' + esc(key) + '=' + esc(vals[i]) + '</code>' : '')).filter(Boolean).join('<br>') ||
            tt('<span class="dim">строки нет — действует значение по умолчанию в коде</span>', '<span class="dim">no line — the code\'s default applies</span>');
        const row = (k, val) => (val ? '<div class="kv"><span>' + k + '</span><div>' + val + '</div></div>' : '');
        const help = $('#view-adv .help');
        help.innerHTML =
            '<div class="help-sec">' + secById[o.section].n + ' · ' + esc(secById[o.section].label) + '</div>' +
            '<h3>' + esc(o.label) + '</h3>' +
            (b ? tt('<div class="warn">Сейчас не действует: нужно — ', '<div class="warn">No effect now: needs ') + esc(b) + '</div>' : '') +
            '<p>' + esc(o.hint || '') + '</p>' +
            // A choice's own description (e.g. the DLAA models): the current one.
            (o.type === 'select' && o.choices ? row(tt('Этот вариант', 'This choice'), esc((o.choices.find(c => c.v === v) || {}).desc || '')) : '') +
            row(tt('Влияет на', 'Affects'), esc(o.affects || '')) +
            row(tt('Цена', 'Cost'), o.cost ? esc(o.cost) : tt('<span class="dim">незаметна</span>', '<span class="dim">negligible</span>')) +
            row(tt('В игре', 'In game'), o.hotkey ? '<kbd>' + esc(o.hotkey) + tt('</kbd> <span class="dim">до выхода из игры</span>', '</kbd> <span class="dim">until the game exits</span>') : '') +
            row(tt('Требует', 'Needs'), chain.length ? esc(chain.join(' → ')) : '') +
            row(tt('Шейдеры', 'Shaders'), o.shaders ? tt('да — ставятся вместе с GI', 'yes — installed with the GI') : '') +
            row(tt('Сейчас', 'Now'), '<b class="' + (same(v, o.def) ? '' : 'acc') + '">' + esc(valueLabel(o, v)) + '</b>' +
                (same(v, o.def) ? tt(' <span class="dim">(по умолчанию)</span>', ' <span class="dim">(default)</span>') : tt(' <span class="dim">по умолчанию: ', ' <span class="dim">default: ') + esc(valueLabel(o, o.def)) + '</span>')) +
            (S.expert ? row('CryMT.ini', iniDesc) : '');
        if (!same(v, o.def)) {
            const r = el('button', 'btn small ghost', tt('Вернуть по умолчанию', 'Back to default'));
            r.addEventListener('click', () => set(o.id, o.def));
            help.append(r);
        }
        if (!$('#view-adv .ini pre')) return;
        const entries = I.wanted(C, S.values).filter(x => x.val != null);
        $('#view-adv .ini pre').innerHTML = ['CryMT', 'Env', 'CVars', 'Launcher'].map(sec => {
            const mine = entries.filter(x => x.sec === sec);
            return mine.length ? '<span class="ln sec">[' + sec + ']</span>' + mine.map(x =>
                '<span class="ln' + (x.opt === o.id ? ' cur' : '') + (x.opt && !same(S.values[x.opt], byId[x.opt].def) ? ' chg' : '') + '"' +
                (x.opt ? ' data-id="' + x.opt + '"' : '') + '><b>' + esc(x.key) + '</b>=' + esc(x.val) + '</span>').join('') : '';
        }).join('');
        $$('#view-adv .ini .ln[data-id]').forEach(l => l.addEventListener('click', () => gotoOpt(l.dataset.id)));
    }
    function gotoOpt(id) {
        const o = byId[id];
        if (S.tab !== 'adv') showTab('adv');
        if (o.expert && !S.expert && !S.query) { S.expert = true; buildAdv(); }
        if (S.query && !matches(o, S.query)) { S.query = ''; $('#view-adv .search').value = ''; buildList(); }
        S.focus = id;
        save();
        sync();
        const r = $('#view-adv .opt[data-id="' + id + '"]');
        if (r) {
            const body = $('#view-adv .list-body');
            body.scrollTo({top: r.offsetTop - body.clientHeight / 2 + r.offsetHeight / 2, behavior: 'smooth'});
            r.classList.remove('flash');
            void r.offsetWidth;
            r.classList.add('flash');
        }
    }

    // ---------------------------------------------------------------- keys
    function buildKeys() {
        const v = $('#view-keys');
        v.innerHTML = tt('<div class="keys-intro panel cut"><h3>Клавиши в игре</h3><p>Панель CryMT в углу экрана показывает, что включено. ', '<div class="keys-intro panel cut"><h3>Keys in game</h3><p>The CryMT panel in the corner of the screen shows what is on. ') +
            tt('Клавиши меняют настройки на лету — для сравнения «было / стало». Изменения действуют до выхода из игры; ', 'The keys change settings on the fly, to compare before / after. Changes last until the game exits; ') +
            tt('постоянные — здесь, в лаунчере.</p><div class="keys-off warn"></div></div>', 'the lasting ones are made here, in the launcher.</p><div class="keys-off warn"></div></div>');
        const grid = el('div', 'keys');
        C.HOTKEYS.forEach(g => {
            const p = el('div', 'panel cut keygroup');
            p.innerHTML = '<h3>' + esc(g.group) + '</h3>' + g.keys.map(([k, text, id]) =>
                '<div class="key' + (id ? ' link' : '') + '"' + (id ? ' data-id="' + id + '"' : '') + '><span class="caps">' +
                k.split(' / ').map(x => '<kbd>' + esc(x) + '</kbd>').join('<i>/</i>') + '</span><span class="key-text">' + esc(text) + '</span>' +
                (id ? '<span class="key-now"></span>' : '') + '</div>').join('');
            grid.append(p);
        });
        v.append(grid);
        $$('#view-keys .key.link').forEach(k => k.addEventListener('click', () => gotoOpt(k.dataset.id)));
    }
    function syncKeys() {
        const kb = blocker(byId.keys, S.values);
        const off = $('#view-keys .keys-off');
        off.textContent = kb ? tt('Сейчас клавиши не работают: нужно — ', 'The keys do not work now: needs ') + kb + '.' : S.values.keys ? '' : tt('Сейчас клавиши выключены: опция «Клавиши CryMT в игре» на главной.', 'The keys are off now: option «CryMT keys in game» on the Home tab.');
        off.style.display = off.textContent ? '' : 'none';
        $$('#view-keys .key.link').forEach(k => {
            const o = byId[k.dataset.id];
            const b = blocker(o, S.values);
            $('.key-now', k).textContent = b ? tt('не действует', 'no effect') : tt('при запуске: ', 'at start: ') + valueLabel(o, S.values[o.id]);
            k.classList.toggle('off', !!b);
        });
    }

    // ---------------------------------------------------------------- news / updates
    function buildNews() {
        const st = C.STATUS, u = S.upd;
        const v = $('#view-news');
        v.innerHTML = '';
        const wrap = el('div', 'news');
        const list = el('div', 'panel cut news-list');
        const day = iso => (iso ? iso.slice(8, 10) + '.' + iso.slice(5, 7) + '.' + iso.slice(0, 4) : '');
        // The release notes' section in the launcher's language ("## Русский" / "## English"), whole, as simple HTML.
        const notes = body => {
            const text = String(body || '').replace(/\r/g, '');
            const parts = text.split(/^## +/m);
            const mine = parts.find(x => x.startsWith(lang === 'en' ? 'English' : 'Русский' /* not ui */));
            const lines = (mine ? mine.slice(mine.indexOf('\n') + 1) : text).split('\n');
            let html = '', inList = false;
            const inline = t => esc(t).replace(/\*\*(.+?)\*\*/g, '<b>$1</b>').replace(/`([^`]+)`/g, '<code>$1</code>');
            for (const raw of lines) {
                const item = raw.match(/^(\s*)(?:[-*]|\d+\.)\s+(.*)$/);
                if (item) {
                    if (!inList) { html += '<ul class="rel">'; inList = true; }
                    html += '<li' + (item[1].length >= 2 ? ' class="sub"' : '') + '>' + inline(item[2]) + '</li>';
                    continue;
                }
                if (inList) { html += '</ul>'; inList = false; }
                if (raw.trim()) html += '<p>' + inline(raw.trim()) + '</p>';
            }
            return html + (inList ? '</ul>' : '');
        };
        const rels = (u.releases || []).filter(r => !r.draft && (S.channel === 'test' || !r.prerelease)).slice(0, 8);
        list.innerHTML = tt('<h3>Что нового</h3>', '<h3>What\'s new</h3>') + (rels.length
            ? rels.map(r => '<article><time>' + esc(day(r.published_at)) + '</time><div><b>' + esc(r.name || r.tag_name) +
                (r.prerelease ? tt(' <span class="chip">тест</span>', ' <span class="chip">test</span>') : '') + '</b>' + notes(r.body) + '</div></article>').join('')
            : C.NEWS.map(n => '<article><time>' + esc(n.date) + '</time><div><b>' + esc(n.title) + '</b><p>' + esc(n.text) + '</p></div></article>').join(''));
        const side = el('div', 'panel cut news-side');
        const installed = st.installed ? 'v' + st.installed + (st.installedDate ? ' · ' + st.installedDate : '') : tt('не через лаунчер', 'not by the launcher');
        let box = '';
        if (u.state === 'checking' || u.state === 'verifying') box = tt('<div class="kv"><span>Обновления</span><div class="dim">проверяю…</div></div>', '<div class="kv"><span>Updates</span><div class="dim">checking…</div></div>');
        else if (u.state === 'available' && u.manifest)
            box = tt('<div class="upd"><div><b>Доступна v', '<div class="upd"><div><b>Available: v') + esc(u.manifest.version) + (u.release && u.release.prerelease ? tt(' (тест)', ' (test)') : '') + '</b><span>' +
                esc(u.manifest.date) + ' · ' + u.manifest.files + tt(' файлов · ', ' files · ') + (u.manifest.bytes / 1048576).toFixed(1) + tt(' МБ · подпись проверена</span></div></div>', ' MB · signature checked</span></div></div>');
        else if (u.state === 'installing' || u.progress) {
            const p = u.progress || {};
            const names = {download: tt('скачиваю', 'downloading'), unpack: tt('распаковываю', 'unpacking'), check: tt('сверяю файлы', 'checking files'), install: tt('ставлю', 'installing')};
            const pct = p.total ? Math.round(p.done / p.total * 100) : 0;
            box = '<div class="upd"><div><b>' + esc(names[p.stage] || tt('начинаю', 'starting')) + (p.total ? ' · ' + pct + '%' : '') + '</b>' +
                (p.stage === 'download' ? '<span>' + (p.done / 1048576).toFixed(1) + tt(' МБ', ' MB') + (p.total ? tt(' из ', ' of ') + (p.total / 1048576).toFixed(1) : '') + '</span>' : '') +
                '</div>' + meter(p.total ? pct / 100 * MS_SCALE : 0) + '</div>';
        }
        else if (u.state === 'latest') box = tt('<div class="kv"><span>Обновления</span><div>установлена последняя</div></div>', '<div class="kv"><span>Updates</span><div>the latest is installed</div></div>');
        else if (u.state === 'none') box = tt('<div class="kv"><span>Обновления</span><div class="dim">в канале пока нет сборок</div></div>', '<div class="kv"><span>Updates</span><div class="dim">no builds in this channel yet</div></div>');
        else if (u.state === 'restarting') box = tt('<div class="kv"><span>Обновления</span><div>перезапуск…</div></div>', '<div class="kv"><span>Updates</span><div>restarting…</div></div>');
        else if (u.state === 'done')
            box = '<div class="upd"><div><b>' + esc(u.done) + tt(': готово</b><span>', ': done</b><span>') +
                (u.restart ? tt('Перезапустите лаунчер, чтобы закончить', 'Restart the launcher to finish') : tt('Можно играть', 'Ready to play')) + '</span></div></div>';
        else if (u.state === 'error') box = tt('<div class="kv"><span>Обновления</span><div class="warn">', '<div class="kv"><span>Updates</span><div class="warn">') + esc(u.error) + '</div></div>';
        side.innerHTML = tt('<h3>Версия</h3>', '<h3>Version</h3>') +
            tt('<div class="kv"><span>Установлено</span><div>', '<div class="kv"><span>Installed</span><div>') + esc(installed) + '</div></div>' + box +
            tt('<div class="kv"><span>Канал</span><div class="chan"></div></div>', '<div class="kv"><span>Channel</span><div class="chan"></div></div>') +
            '<div class="btns"></div>' +
            tt('<p class="fine">Сборки берутся из релизов github.com/', '<p class="fine">Builds come from the releases of github.com/') + esc(C.UPDATE.repo) + tt('. Перед установкой проверяются подпись списка файлов ', '. Before installing, the signature of the file list ') +
            tt('и SHA-256 каждого файла; заменённые файлы сохраняются для отката. CryMT.ini обновление не трогает.</p>', 'and the SHA-256 of every file are checked; replaced files are kept for rollback. Updates never touch CryMT.ini.</p>');
        $('.chan', side).append(seg([['stable', tt('Стабильный', 'Stable')], ['test', tt('Тестовый', 'Test')]], S.channel, c => {
            S.channel = c;
            save();
            toast(c === 'test' ? tt('Тестовый канал: сборки сразу после правок, могут быть ошибки', 'Test channel: builds right after changes, may have bugs') : tt('Стабильный канал', 'Stable channel'));
            checkUpdates();
        }));
        const btns = $('.btns', side);
        const button = (label, cls, run) => {
            const b = el('button', 'btn' + (cls ? ' ' + cls : ''), esc(label));
            b.disabled = u.state === 'installing' || u.state === 'restarting';
            b.addEventListener('click', run);
            btns.append(b);
        };
        if (u.state === 'available') button(tt('Обновить до v', 'Update to v') + u.manifest.version, '', installUpdate);
        if (u.state === 'done' && u.restart) button(tt('Перезапустить лаунчер', 'Restart the launcher'), '', restartLauncher);
        button(tt('Проверить обновления', 'Check for updates'), 'ghost', checkUpdates);
        button(tt('Проверить файлы', 'Check files'), 'ghost', host.verify);
        button(tt('Откатить', 'Roll back'), 'ghost', host.rollback);
        button(tt('Папка игры', 'Game folder'), 'ghost', () => host.open('game'));
        button(tt('Логи CryMT', 'CryMT logs'), 'ghost', () => host.open('logs'));
        wrap.append(list, side);
        v.append(wrap);
    }

    // ---------------------------------------------------------------- footer
    function syncFooter() {
        const st = C.STATUS, vals = S.values;
        const dot = cls => '<i class="dot ' + cls + '"></i>';
        const shd = shadersNeeded(vals) ? dot('acc') + tt('Шейдеры GI ставятся при запуске', 'GI shaders are installed at start') : dot('dim') + tt('Шейдеры не нужны', 'No shaders needed');
        $('#status').innerHTML =
            '<span title="' + esc(st.game) + '">' + dot(st.buildOk ? 'ok' : 'bad') + tt('Crysis · сборка ', 'Crysis · build ') + esc(st.build) + (st.buildOk ? '' : tt(' — нужна 6156 (патч 1.2.1)', ' — needs 6156 (patch 1.2.1)')) + '</span>' +
            '<span' + (st.launcherWarn ? ' title="' + esc(tt('c1-launcher v8 исправляет больше ошибок игры: https://github.com/ccomrade/c1-launcher/releases', 'c1-launcher v8 fixes more of the game\'s bugs: https://github.com/ccomrade/c1-launcher/releases')) + '"' : '') + '>' +
                dot(st.launcherOk ? (st.launcherWarn ? 'warn' : 'ok') : 'bad') + esc(st.launcher) + '</span>' +
            (st.proxy === false ? '<span>' + dot('bad') + tt('Нет Bin64\\dbghelp.dll</span>', 'No Bin64\\dbghelp.dll</span>') : '') +
            '<span>' + shd + '</span>' +
            (modOf(vals) ? '<span' + (modOf(vals).gi && MODES.dx12Mode(vals) && vals.gi ? ' class="warn"' : '') + '>' + dot(modOf(vals).gi && MODES.dx12Mode(vals) && vals.gi ? 'warn' : 'acc') +
                tt('Мод: ', 'Mod: ') + esc(modOf(vals).name) + (modOf(vals).gi && MODES.dx12Mode(vals) && vals.gi ? (modGiRisk(vals) ? tt(' · GI на свой риск', ' · GI at own risk') : tt(' · GI выкл', ' · GI off')) : '') + '</span>' : '') +
            (msaaOff(vals) ? '<span class="warn" title="' + esc(tt('MSAA из настроек игры с CryMT выключается (пока не поддерживается)', 'The game\'s MSAA is turned off with CryMT (not supported yet)')) + '">' +
                dot('warn') + tt('MSAA выключен', 'MSAA off') + '</span>' : '') +
            (st.lastOff ? '<span class="warn" title="' + esc(st.lastOff) + '">' + dot('warn') + tt('Прошлый запуск: CryMT был выключен</span>', 'Last start: CryMT was off</span>') : '') +
            (S.upd.state === 'available' ? '<button class="linkish" type="button" id="updLink">' + dot('acc') + tt('Есть обновление v', 'Update available: v') +
                esc(S.upd.manifest.version) + '</button>' : '') +
            (S.upd.state === 'done' && S.upd.restart ? '<button class="linkish" type="button" id="restartLink">' + dot('acc') +
                tt('Обновлено — перезапустить лаунчер', 'Updated — restart the launcher') + '</button>' : '');
        const u = $('#updLink');
        if (u) u.addEventListener('click', () => showTab('news'));
        const r = $('#restartLink');
        if (r) r.addEventListener('click', restartLauncher);
        $$('.tab').forEach(b => b.classList.toggle('badge', b.dataset.tab === 'news' && (S.upd.state === 'available' || (S.upd.state === 'done' && S.upd.restart))));
        const cur = currentPreset(vals);
        const ms = loadMs(vals);
        $('#barLoad').innerHTML = '<div><span>' + esc(cur.preset ? cur.preset.label : tt('Свои настройки', 'Own settings')) + (cur.diff ? tt(' · изменено ', ' · changed ') + cur.diff : '') +
            '</span><b>' + (ms ? '≈ ' + fmt(ms) + tt(' мс', ' ms') : '—') + '</b></div>' + meter(ms);
        const play = $('#play');
        play.disabled = S.busy;
        play.textContent = S.busy ? tt('Запуск…', 'Starting…') : tt('Играть', 'Play');
    }

    // ---------------------------------------------------------------- sync everything that depends on values
    function sync() {
        if (S.tab === 'home') syncHome();
        if (S.tab === 'adv') {
            $$('#view-adv .opt').forEach(syncRow);
            syncAdvSide();
            syncNav();
            $$('#view-adv .nav-exp .tg').forEach(t => t.setAttribute('aria-pressed', S.expert ? 'true' : 'false'));
        }
        if (S.tab === 'keys') syncKeys();
        syncFooter();
    }

    buildHeader();
    applyLangChrome();
    buildHome();
    buildAdv();
    buildKeys();
    buildNews();
    showTab(TABS().some(t => t[0] === S.tab) ? S.tab : 'home');
    if (!wv) checkUpdates(); // in a plain browser too (the window checks once it has read the game's state)
})();
