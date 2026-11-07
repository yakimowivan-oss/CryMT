// CryMT launcher UI. Everything shown comes from options.js, CryMT.ini goes through ini.js; this file draws and talks to
// the window (CryMTLauncher.exe, WebView2): messages 'load', 'save\n<ini>', 'play\n<gi>\n<args>\n<after>', 'drag',
// 'min', 'close', 'open\n<what>'; replies {type: 'state' | 'saved' | 'played' | 'error'}. Opened as a plain page in a
// browser it works on localStorage and shows what the window would do.
(function () {
    'use strict';
    const C = window.CRYMT, I = window.CryIni;
    const byId = {};
    C.OPTIONS.forEach(o => (byId[o.id] = o));
    const secById = {};
    C.SECTIONS.forEach((s, i) => (secById[s.id] = Object.assign({n: String(i + 1).padStart(2, '0')}, s)));
    const PERSONAL = C.OPTIONS.filter(o => o.personal).map(o => o.id); // kept when a preset is picked
    const STORE = 'crymt.launcher.v2';
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
        const saved = JSON.parse(localStorage.getItem(STORE) || 'null');
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
        try { localStorage.setItem(STORE, JSON.stringify({values: S.values, tab: S.tab, expert: S.expert, focus: S.focus, channel: S.channel})); } catch (e) { /* ignore */ }
    }

    // ---------------------------------------------------------------- logic
    const MODES = {
        mtMode: v => v.mode !== 'off',
        dx12Mode: v => v.mode === 'dx12' || v.mode === 'dlaa',
        dlaaMode: v => v.mode === 'dlaa',
    };
    const MODE_NEED = {mtMode: 'режим с CryMT', dx12Mode: 'режим DirectX 12 или DLAA', dlaaMode: 'режим DLAA'};
    function isOn(o, v) {
        if (o.type === 'toggle') return !!v;
        if (o.type === 'select') return v !== 'off' && v !== o.offValue;
        return true;
    }
    // What has to be turned on for this option to matter (null: it works now). Launcher options always work.
    function blocker(o, vals) {
        if (o.ini[0] === 'Launcher') return null;
        if (o.id !== 'mode' && vals.mode === 'off') return 'режим с CryMT';
        if (!o.needs) return null;
        if (MODES[o.needs]) return MODES[o.needs](vals) ? null : MODE_NEED[o.needs];
        const p = byId[o.needs];
        return blocker(p, vals) || (isOn(p, vals[p.id]) ? null : p.label);
    }
    const featureOn = (id, vals) => !blocker(byId[id], vals) && isOn(byId[id], vals[id]);
    function loadMs(vals) {
        let ms = 0;
        C.OPTIONS.forEach(o => { if (o.ms && !blocker(o, vals)) ms += o.ms(vals[o.id]); });
        return ms;
    }
    const shadersNeeded = vals => featureOn('gi', vals);
    const launchArgs = vals => '-dx10' + (vals.devmode ? ' -DEVMODE' : '');
    function valueLabel(o, v) {
        if (o.type === 'toggle') return v ? 'вкл' : 'выкл';
        if (o.type === 'select') return (o.choices.find(c => c.v === v) || {label: v}).label;
        return fmt(v) + (o.unit ? ' ' + o.unit : '');
    }
    function needChain(o) {
        const out = [];
        for (let n = o.needs; n;) {
            if (MODES[n]) { out.push(MODE_NEED[n]); break; }
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
    const FEATURES = [
        ['Многопоточность', v => v.mode !== 'off'],
        ['DirectX 12', v => MODES.dx12Mode(v)],
        ['DLAA', v => MODES.dlaaMode(v)],
        ['Глобальное освещение', v => featureOn('gi', v)],
        ['GTAO', v => featureOn('gtao', v)],
        ['Контактные тени', v => featureOn('contact', v)],
        ['GI для дыма и пыли', v => featureOn('particles', v)],
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
        drag: () => send('drag'),
        min: () => send('min'),
        close: () => { host.save(); send('close'); },
        open: what => send('open\n' + what),
        fetch: (id, url) => send('fetch\n' + id + '\n' + url),
        manifest: (url, sig) => send('update-manifest\n' + url + '\n' + sig),
        install: zip => send('update-install\n' + zip),
        rollback: () => send('rollback'),
        verify: () => send('verify'),
    } : {
        save: () => {},
        saveSoon: () => {},
        play: () => toast('В окне лаунчера: записать CryMT.ini → ' + (shadersNeeded(S.values) ? 'поставить шейдеры GI → ' : 'убрать шейдеры GI → ') +
            'запустить Bin64\\Crysis.exe ' + launchArgs(S.values)),
        drag: () => {},
        min: () => toast('Свернуть — в окне лаунчера'),
        close: () => toast('Закрыть — в окне лаунчера'),
        open: what => toast('Открыть папку «' + what + '» — в окне лаунчера'),
        // In a plain browser the release list still comes from GitHub (its API allows pages of any origin).
        fetch: (id, url) => window.fetch(url).then(r => r.text().then(body => onHost({type: 'fetched', id, ok: r.ok, status: r.status, body})),
                                                   e => onHost({type: 'fetched', id, ok: false, status: 0, error: String(e), body: ''})),
        manifest: () => onHost({type: 'manifest', ok: false, error: 'подпись проверяет окно лаунчера'}),
        install: () => toast('Обновление ставит окно лаунчера'),
        rollback: () => toast('Откат — в окне лаунчера'),
        verify: () => toast('Проверка файлов — в окне лаунчера'),
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
            if (m.exists) S.values = I.readValues(C, S.iniText);
            Object.assign(C.STATUS, m.status || {});
            if (m.reason === 'external') toast('CryMT.ini изменён вне лаунчера — настройки перечитаны');
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
            toast(m.ok ? 'Игра запущена' : 'Не удалось запустить: ' + m.error);
        } else if (m.type === 'fetched' && m.id === 'releases') {
            if (!m.ok) Object.assign(S.upd, {state: 'error', error: 'GitHub не ответил: ' + (m.error || m.status)});
            else {
                try {
                    S.upd.releases = JSON.parse(m.body);
                    pickRelease();
                } catch (e) {
                    Object.assign(S.upd, {state: 'error', error: 'ответ GitHub не читается'});
                }
            }
            buildNews();
            syncFooter();
        } else if (m.type === 'manifest') {
            if (m.ok) Object.assign(S.upd, {state: 'available', manifest: parseManifest(m.text)});
            if (m.ok && autoAction === 'update') setTimeout(() => $$('#view-news .btns .btn')[0].click(), 500);
            else Object.assign(S.upd, {state: 'error', error: m.error});
            buildNews();
            syncFooter();
        } else if (m.type === 'progress') {
            S.upd.progress = m;
            buildNews();
        } else if (m.type === 'installed' || m.type === 'rolledback') {
            const what = m.type === 'installed' ? 'Обновление до v' + m.version : 'Откат';
            if (m.ok) {
                Object.assign(S.upd, {state: m.restart ? 'restarting' : 'done', progress: null});
                toast(what + ': готово' + (m.restart ? ' — лаунчер перезапускается' : ''));
                send('load');
            } else {
                Object.assign(S.upd, {state: 'error', error: m.error, progress: null});
                toast(what + ': ' + m.error);
            }
            buildNews();
            syncFooter();
        } else if (m.type === 'verified') {
            toast(!m.ok ? 'Нет CryMT\\version.txt: файлы ставились не через лаунчер' :
                m.wrong ? 'Проверено файлов: ' + m.checked + ', не совпало: ' + m.wrong + ' (' + m.bad.join(', ') + ')' :
                'Проверено файлов: ' + m.checked + ', все совпали с версией v' + C.STATUS.installed);
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
        if (msg) toast(msg, {label: 'Отменить', run: () => { S.values = prev; changed(); }});
    }
    function set(id, v) {
        S.values[id] = v;
        S.focus = id;
        changed();
    }
    function seg(items, cur, onPick) {
        const box = el('div', 'seg');
        items.forEach(([v, label]) => {
            const b = el('button', v === cur ? 'on' : '', esc(label));
            b.type = 'button';
            b.dataset.v = v;
            b.addEventListener('click', () => onPick(v));
            box.append(b);
        });
        return box;
    }
    function toggleButton(on, onClick) {
        const b = el('button', 'tg', '<span>Выкл</span><span>Вкл</span>');
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
        } else if (o.type === 'select') {
            wrap.append(seg(o.choices.map(c => [c.v, c.label]), S.values[o.id], v => set(o.id, v)));
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
            d.title = 'по умолчанию: ' + valueLabel(o, o.def);
            track.append(r, d);
            const val = el('button', 'sl-v');
            val.type = 'button';
            val.title = 'Двойной щелчок — значение по умолчанию';
            val.addEventListener('dblclick', () => set(o.id, o.def));
            box.append(track, val);
            wrap.append(box);
        }
        return wrap;
    }
    function syncControl(wrap, o) {
        const v = S.values[o.id];
        if (o.type === 'toggle') $('.tg', wrap).setAttribute('aria-pressed', v ? 'true' : 'false');
        else if (o.type === 'select') $$('.seg button', wrap).forEach(b => b.classList.toggle('on', b.dataset.v === v));
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
        if (o.hotkey) h += '<kbd title="Клавиша в игре">' + esc(o.hotkey) + '</kbd>';
        if (o.cost) h += '<span class="chip cost" title="Цена на видеокарте">' + esc(o.cost) + '</span>';
        if (o.shaders) h += '<span class="chip shd" title="Нужны шейдеры CryMT">шейдеры</span>';
        if (o.expert) h += '<span class="chip exp" title="Для энтузиастов">тонкая</span>';
        return h;
    }
    const meter = ms => '<span class="meter"><i style="width:' + Math.min(100, (ms / MS_SCALE) * 100) + '%"></i></span>';

    // ---------------------------------------------------------------- title bar and tabs
    const TABS = [['home', 'Главная'], ['adv', 'Настройки'], ['keys', 'Клавиши'], ['news', 'Обновления']];
    function buildHeader() {
        const tabs = $('#tabs');
        TABS.forEach(([id, label]) => {
            const b = el('button', 'tab', '<span>' + esc(label) + '</span>');
            b.type = 'button';
            b.dataset.tab = id;
            b.setAttribute('role', 'tab');
            b.addEventListener('click', () => showTab(id));
            tabs.append(b);
        });
        // The header is the window's title bar: dragging its empty parts moves the window.
        $('.top').addEventListener('mousedown', e => {
            if (e.button === 0 && !e.target.closest('button, input, a, [role="button"]')) { e.preventDefault(); host.drag(); }
        });
        $('#winMin').addEventListener('click', host.min);
        $('#winClose').addEventListener('click', host.close);
        $('#play').addEventListener('click', () => { if (!S.busy) host.play(); });
        document.documentElement.classList.toggle('in-host', !!wv);
    }
    function showTab(id) {
        S.tab = id;
        save();
        $$('.tab').forEach(b => b.classList.toggle('on', b.dataset.tab === id));
        $$('.view').forEach(v => v.classList.toggle('on', v.id === 'view-' + id));
        sync();
    }

    // ---------------------------------------------------------------- home (presets)
    function buildHome() {
        const v = $('#view-home');
        v.innerHTML = '<div class="home-head"><h2>Режим запуска</h2><p>Выберите готовый набор. Всё тонкое — во вкладке «Настройки», ' +
            'там же подсказки, что за что отвечает.</p></div>';
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
                '<ul class="feats">' + FEATURES.map(([n, f]) => '<li class="' + (f(pv) ? 'on' : '') + '"><i></i>' + esc(n) + '</li>').join('') + '</ul>' +
                '<div class="card-meta"><span>Видеокарта</span><b>' + (ms ? '≈ ' + fmt(ms) + ' мс' : '—') + '</b></div>' + meter(ms) +
                '<div class="card-meta"><span>Шейдеры</span><b>' + esc(p.shaders) + '</b></div>' +
                '<div class="card-meta"><span>Village, FPS</span><b>' + (p.fps && p.fps.village ? fmt(p.fps.village) : 'не замерено') + '</b></div>' +
                '<div class="card-badge"></div>';
            const pick = () => {
                const cur = currentPreset(S.values);
                if (cur.preset === p && cur.diff === 0) return;
                const custom = cur.diff > 0 || !cur.preset;
                setValues(presetValues(p), custom ? 'Режим «' + p.label + '»: свои настройки сброшены' : null);
            };
            card.addEventListener('click', pick);
            card.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } });
            grid.append(card);
        });
        v.append(grid);
        const low = el('div', 'home-low');
        low.append(el('div', 'panel cut detail'));
        const quick = el('div', 'panel cut quick');
        quick.append(el('h3', null, 'Окно и управление'));
        PERSONAL.filter(id => !byId[id].expert).forEach(id => quick.append(optRow(byId[id], true)));
        const adv = el('button', 'btn ghost', 'Тонкая настройка');
        adv.addEventListener('click', () => showTab('adv'));
        quick.append(adv);
        low.append(quick);
        v.append(low);
    }
    function syncHome() {
        const cur = currentPreset(S.values);
        $$('#view-home .card').forEach(c => {
            const mine = cur.preset && cur.preset.id === c.dataset.p;
            c.classList.toggle('on', !!mine);
            $('.card-badge', c).innerHTML = mine ? (cur.diff ? 'выбрано · изменено: ' + cur.diff : 'выбрано') : '';
        });
        const vals = S.values;
        const rows = C.OPTIONS.filter(o => o.ms && !blocker(o, vals) && o.ms(vals[o.id]) > 0);
        const ms = loadMs(vals);
        const notes = [];
        if (vals.mode === 'off') notes.push('CryMT не загружается: игра запускается как есть, со своими настройками.');
        if (MODES.mtMode(vals)) notes.push('Многопоточность ускоряет процессорную часть кадра; на видеокарту не влияет.');
        if (MODES.dlaaMode(vals)) notes.push('DLAA работает только на видеокартах NVIDIA RTX.');
        if (shadersNeeded(vals)) notes.push('GI ставит 5 файлов шейдеров (Game\\zzCryMT.pak). Со сторонними шейдерами GI не совместим — для них «Только MT» или «MT + DLAA».');
        $('#view-home .detail').innerHTML =
            '<h3>Сейчас: ' + esc(cur.preset ? cur.preset.label : 'свои настройки') + (cur.diff ? ' <span class="chip">изменено: ' + cur.diff + '</span>' : '') + '</h3>' +
            '<div class="cost-list">' + rows.map(o => '<div><span>' + esc(o.label) + (o.type === 'select' ? ' · ' + esc(valueLabel(o, vals[o.id])) : '') +
                '</span><b>' + fmt(o.ms(vals[o.id])) + ' мс</b></div>').join('') +
            '<div class="total"><span>Итого на видеокарте</span><b>' + (ms ? '≈ ' + fmt(ms) + ' мс' : '0') + '</b></div></div>' +
            '<p class="fine">Оценка по замерам: демо village разработчиков, 5120×1440. При 60 FPS кадр — 16.7 мс.</p>' +
            '<ul class="notes">' + notes.map(n => '<li>' + esc(n) + '</li>').join('') + '</ul>';
        $$('#view-home .quick .opt').forEach(syncRow);
    }

    // ---------------------------------------------------------------- advanced
    function optRow(o, compact) {
        const row = el('div', 'opt' + (compact ? ' compact' : ''));
        row.dataset.id = o.id;
        row.innerHTML = '<div class="opt-main"><div class="opt-label">' + esc(o.label) + (compact ? '' : '<span class="badges">' + badges(o) + '</span>') +
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
        $('.opt-why', row).textContent = b ? 'не действует: нужно — ' + b : '';
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
        const wrap = el('div', 'adv');
        const nav = el('aside', 'nav panel cut');
        nav.append(el('div', 'nav-title', 'Разделы'));
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
        exp.append(el('div', 'nav-exp-text', '<b>Для энтузиастов</b><span>все параметры, включая отладочные</span>'));
        exp.append(toggleButton(S.expert, () => { S.expert = !S.expert; save(); buildList(); sync(); }));
        nav.append(exp);
        const reset = el('button', 'btn ghost', 'Всё по умолчанию');
        reset.addEventListener('click', () => setValues(presetValues(C.PRESETS[C.PRESETS.length - 1]), 'Все настройки — по умолчанию'));
        nav.append(reset);

        const list = el('div', 'list panel cut');
        const head = el('div', 'list-head');
        const search = el('input', 'search');
        search.type = 'search';
        search.placeholder = 'Поиск: опция, ключ ini, клавиша, «трава», «дым»…';
        search.value = S.query;
        search.addEventListener('input', () => { S.query = search.value.trim(); buildList(); sync(); });
        head.append(search);
        list.append(head, el('div', 'list-body'));
        $('.list-body', list).addEventListener('scroll', syncNav);

        const side = el('aside', 'side');
        side.append(el('div', 'help panel cut'));
        const ini = el('div', 'ini panel cut');
        ini.innerHTML = '<div class="ini-head"><span>CryMT.ini</span><span class="ini-btns"><button class="btn small ghost" type="button" data-a="copy">Копировать</button>' +
            '<button class="btn small ghost" type="button" data-a="folder">Папка</button></span></div><pre></pre>';
        $('[data-a="copy"]', ini).addEventListener('click', () => {
            const text = iniText(S.values);
            const done = () => toast('CryMT.ini скопирован');
            if (navigator.clipboard) navigator.clipboard.writeText(text).then(done, () => toast('Буфер обмена недоступен'));
            else toast('Буфер обмена недоступен');
        });
        $('[data-a="folder"]', ini).addEventListener('click', () => host.open('crymt'));
        side.append(ini);
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
                (hidden ? '<button class="sec-more" type="button">+' + hidden + ' для энтузиастов</button>' : '') + '</h3>';
            if (hidden) $('.sec-more', sec).addEventListener('click', () => { S.expert = true; save(); buildAdv(); sync(); });
            vis.forEach(o => sec.append(optRow(o)));
            body.append(sec);
        });
        if (!shown) body.innerHTML = '<div class="empty-list">Ничего не нашлось по «' + esc(q) + '»</div>';
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
            '<span class="dim">строки нет — действует значение по умолчанию в коде</span>';
        const row = (k, val) => (val ? '<div class="kv"><span>' + k + '</span><div>' + val + '</div></div>' : '');
        const help = $('#view-adv .help');
        help.innerHTML =
            '<div class="help-sec">' + secById[o.section].n + ' · ' + esc(secById[o.section].label) + '</div>' +
            '<h3>' + esc(o.label) + '</h3>' +
            (b ? '<div class="warn">Сейчас не действует: нужно — ' + esc(b) + '</div>' : '') +
            '<p>' + esc(o.hint || '') + '</p>' +
            row('Влияет на', esc(o.affects || '')) +
            row('Цена', o.cost ? esc(o.cost) : '<span class="dim">незаметна</span>') +
            row('В игре', o.hotkey ? '<kbd>' + esc(o.hotkey) + '</kbd> <span class="dim">до выхода из игры</span>' : '') +
            row('Требует', chain.length ? esc(chain.join(' → ')) : '') +
            row('Шейдеры', o.shaders ? 'да — ставятся вместе с GI' : '') +
            row('Сейчас', '<b class="' + (same(v, o.def) ? '' : 'acc') + '">' + esc(valueLabel(o, v)) + '</b>' +
                (same(v, o.def) ? ' <span class="dim">(по умолчанию)</span>' : ' <span class="dim">по умолчанию: ' + esc(valueLabel(o, o.def)) + '</span>')) +
            row('CryMT.ini', iniDesc);
        if (!same(v, o.def)) {
            const r = el('button', 'btn small ghost', 'Вернуть по умолчанию');
            r.addEventListener('click', () => set(o.id, o.def));
            help.append(r);
        }
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
        v.innerHTML = '<div class="keys-intro panel cut"><h3>Клавиши в игре</h3><p>Панель CryMT в углу экрана показывает, что включено. ' +
            'Клавиши меняют настройки на лету — для сравнения «было / стало». Изменения действуют до выхода из игры; ' +
            'постоянные — здесь, в лаунчере.</p><div class="keys-off warn"></div></div>';
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
        off.textContent = kb ? 'Сейчас клавиши не работают: нужно — ' + kb + '.' : S.values.keys ? '' : 'Сейчас клавиши выключены: опция «Клавиши CryMT в игре» на главной.';
        off.style.display = off.textContent ? '' : 'none';
        $$('#view-keys .key.link').forEach(k => {
            const o = byId[k.dataset.id];
            const b = blocker(o, S.values);
            $('.key-now', k).textContent = b ? 'не действует' : 'при запуске: ' + valueLabel(o, S.values[o.id]);
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
        const clean = t => String(t || '').replace(/[#*_`>]/g, '').replace(/\r/g, '').trim();
        const rels = (u.releases || []).filter(r => !r.draft && (S.channel === 'test' || !r.prerelease)).slice(0, 8);
        list.innerHTML = '<h3>Что нового</h3>' + (rels.length
            ? rels.map(r => '<article><time>' + esc(day(r.published_at)) + '</time><div><b>' + esc(r.name || r.tag_name) +
                (r.prerelease ? ' <span class="chip">тест</span>' : '') + '</b><p>' + esc(clean(r.body).slice(0, 600)).replace(/\n/g, '<br>') +
                '</p></div></article>').join('')
            : C.NEWS.map(n => '<article><time>' + esc(n.date) + '</time><div><b>' + esc(n.title) + '</b><p>' + esc(n.text) + '</p></div></article>').join(''));
        const side = el('div', 'panel cut news-side');
        const installed = st.installed ? 'v' + st.installed + (st.installedDate ? ' · ' + st.installedDate : '') : 'не через лаунчер';
        let box = '';
        if (u.state === 'checking' || u.state === 'verifying') box = '<div class="kv"><span>Обновления</span><div class="dim">проверяю…</div></div>';
        else if (u.state === 'available' && u.manifest)
            box = '<div class="upd"><div><b>Доступна v' + esc(u.manifest.version) + (u.release && u.release.prerelease ? ' (тест)' : '') + '</b><span>' +
                esc(u.manifest.date) + ' · ' + u.manifest.files + ' файлов · ' + (u.manifest.bytes / 1048576).toFixed(1) + ' МБ · подпись проверена</span></div></div>';
        else if (u.state === 'installing' || u.progress) {
            const p = u.progress || {};
            const names = {download: 'скачиваю', unpack: 'распаковываю', check: 'сверяю файлы', install: 'ставлю'};
            const pct = p.total ? Math.round(p.done / p.total * 100) : 0;
            box = '<div class="upd"><div><b>' + esc(names[p.stage] || 'начинаю') + (p.total ? ' · ' + pct + '%' : '') + '</b>' +
                (p.stage === 'download' ? '<span>' + (p.done / 1048576).toFixed(1) + ' МБ' + (p.total ? ' из ' + (p.total / 1048576).toFixed(1) : '') + '</span>' : '') +
                '</div>' + meter(p.total ? pct / 100 * MS_SCALE : 0) + '</div>';
        }
        else if (u.state === 'latest') box = '<div class="kv"><span>Обновления</span><div>установлена последняя</div></div>';
        else if (u.state === 'none') box = '<div class="kv"><span>Обновления</span><div class="dim">в канале пока нет сборок</div></div>';
        else if (u.state === 'restarting') box = '<div class="kv"><span>Обновления</span><div>перезапуск…</div></div>';
        else if (u.state === 'done') box = '<div class="kv"><span>Обновления</span><div>установлено</div></div>';
        else if (u.state === 'error') box = '<div class="kv"><span>Обновления</span><div class="warn">' + esc(u.error) + '</div></div>';
        side.innerHTML = '<h3>Версия</h3>' +
            '<div class="kv"><span>Установлено</span><div>' + esc(installed) + '</div></div>' + box +
            '<div class="kv"><span>Канал</span><div class="chan"></div></div>' +
            '<div class="btns"></div>' +
            '<p class="fine">Сборки берутся из релизов github.com/' + esc(C.UPDATE.repo) + '. Перед установкой проверяются подпись списка файлов ' +
            'и SHA-256 каждого файла; заменённые файлы сохраняются для отката. CryMT.ini обновление не трогает.</p>';
        $('.chan', side).append(seg([['stable', 'Стабильный'], ['test', 'Тестовый']], S.channel, c => {
            S.channel = c;
            save();
            toast(c === 'test' ? 'Тестовый канал: сборки сразу после правок, могут быть ошибки' : 'Стабильный канал');
            checkUpdates();
        }));
        const btns = $('.btns', side);
        const button = (label, cls, run) => {
            const b = el('button', 'btn' + (cls ? ' ' + cls : ''), esc(label));
            b.disabled = u.state === 'installing' || u.state === 'restarting';
            b.addEventListener('click', run);
            btns.append(b);
        };
        if (u.state === 'available')
            button('Обновить до v' + u.manifest.version, '', () => {
                Object.assign(S.upd, {state: 'installing', progress: null});
                buildNews();
                host.install(asset(u.release, n => /\.zip$/i.test(n)).browser_download_url);
            });
        button('Проверить обновления', 'ghost', checkUpdates);
        button('Проверить файлы', 'ghost', host.verify);
        button('Откатить', 'ghost', host.rollback);
        button('Папка игры', 'ghost', () => host.open('game'));
        button('Логи CryMT', 'ghost', () => host.open('logs'));
        wrap.append(list, side);
        v.append(wrap);
    }

    // ---------------------------------------------------------------- footer
    function syncFooter() {
        const st = C.STATUS, vals = S.values;
        const dot = cls => '<i class="dot ' + cls + '"></i>';
        const shd = shadersNeeded(vals) ? dot('acc') + 'Шейдеры GI ставятся при запуске' : dot('dim') + 'Шейдеры не нужны';
        $('#status').innerHTML =
            '<span title="' + esc(st.game) + '">' + dot(st.buildOk ? 'ok' : 'bad') + 'Crysis · сборка ' + esc(st.build) + (st.buildOk ? '' : ' — нужна 6156 (патч 1.2.1)') + '</span>' +
            '<span>' + dot(st.launcherOk ? 'ok' : 'bad') + esc(st.launcher) + '</span>' +
            (st.proxy === false ? '<span>' + dot('bad') + 'Нет Bin64\\dbghelp.dll</span>' : '') +
            '<span>' + shd + '</span>' +
            (st.lastOff ? '<span class="warn" title="' + esc(st.lastOff) + '">' + dot('warn') + 'Прошлый запуск: CryMT был выключен</span>' : '') +
            (S.upd.state === 'available' ? '<button class="linkish" type="button" id="updLink">' + dot('acc') + 'Есть обновление v' +
                esc(S.upd.manifest.version) + '</button>' : '');
        const u = $('#updLink');
        if (u) u.addEventListener('click', () => showTab('news'));
        const cur = currentPreset(vals);
        const ms = loadMs(vals);
        $('#barLoad').innerHTML = '<div><span>' + esc(cur.preset ? cur.preset.label : 'Свои настройки') + (cur.diff ? ' · изменено ' + cur.diff : '') +
            '</span><b>' + (ms ? '≈ ' + fmt(ms) + ' мс' : '—') + '</b></div>' + meter(ms);
        const play = $('#play');
        play.disabled = S.busy;
        play.textContent = S.busy ? 'Запуск…' : 'Играть';
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
    buildHome();
    buildAdv();
    buildKeys();
    buildNews();
    showTab(TABS.some(t => t[0] === S.tab) ? S.tab : 'home');
    if (!wv) checkUpdates(); // in a plain browser too (the window checks once it has read the game's state)
})();
