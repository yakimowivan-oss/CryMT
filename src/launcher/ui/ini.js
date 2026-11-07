// CryMT.ini <-> option values, without the UI (node can test it: tools\prof\launchercheck.js).
// Reading follows GetPrivateProfileString: sections and keys case-insensitive, the first line of a key wins, ';' comments.
// Writing changes the file as little as possible: an option whose lines already read as its value is left alone (its
// formatting and comments stay), a changed one has its lines replaced, removed (null: the code's default) or added at
// the end of its section; lines the launcher does not know stay as they are.
(function (root) {
    'use strict';
    const id = (sec, key) => sec.toLowerCase() + '|' + key.toLowerCase();
    const keysOf = o => (Array.isArray(o.ini[0]) ? o.ini : [o.ini]);
    const multi = o => Array.isArray(o.ini[0]);
    const writeOf = (o, v) => (multi(o) ? o.codec.write(v) : [o.codec.write(v)]);
    const same = (a, b) => (typeof a === 'number' && typeof b === 'number' ? Math.abs(a - b) < 1e-6 : a === b);
    const SECTION_ORDER = ['CryMT', 'Env', 'CVars', 'Launcher'];

    function parse(text) {
        const lines = String(text || '').split(/\r?\n/);
        const keys = {};      // id -> {line, value}
        const sections = {};  // section lower -> {name, header, last}
        let sec = null;
        lines.forEach((raw, i) => {
            const s = raw.trim();
            const m = s.match(/^\[([^\]]+)\]/);
            if (m) {
                sec = m[1].trim();
                const k = sec.toLowerCase();
                if (!sections[k]) sections[k] = {name: sec, header: i, last: i};
                return;
            }
            if (sec == null) return;
            if (s !== '' && sections[sec.toLowerCase()].header <= i) sections[sec.toLowerCase()].last = i;
            if (s === '' || s[0] === ';' || s[0] === '#' || !s.includes('=')) return;
            const eq = s.indexOf('=');
            const k = id(sec, s.slice(0, eq).trim());
            if (!(k in keys)) keys[k] = {line: i, value: s.slice(eq + 1).trim()};
        });
        return {lines, keys, sections};
    }
    const getter = p => (sec, key) => (p.keys[id(sec, key)] ? p.keys[id(sec, key)].value : null);

    function valid(o, v) {
        if (v === undefined || v === null) return false;
        if (o.type === 'toggle') return typeof v === 'boolean';
        if (o.type === 'select') return o.choices.some(c => c.v === v);
        return typeof v === 'number' && isFinite(v);
    }
    function readOption(o, get) {
        const raw = keysOf(o).map(([sec, key]) => get(sec, key));
        const v = o.codec.read(multi(o) ? raw : raw[0]);
        return valid(o, v) ? v : o.def;
    }
    // Values the game will use with this file (options without a valid line: what the code does without it).
    function readValues(C, text) {
        const get = getter(parse(text));
        const v = {};
        C.OPTIONS.forEach(o => (v[o.id] = readOption(o, get)));
        return v;
    }

    // [{sec, key, val, opt}] the launcher wants in the file (val null: no line).
    function wanted(C, values) {
        const out = [];
        C.OPTIONS.forEach(o => {
            const vals = writeOf(o, values[o.id]);
            keysOf(o).forEach(([sec, key], i) => out.push({sec, key, val: vals[i], opt: o.id}));
        });
        C.ALWAYS.forEach(([sec, key, val]) => out.push({sec, key, val, always: true}));
        return out;
    }

    function merge(C, text, values) {
        if (!String(text || '').trim()) return fresh(C, values);
        const p = parse(text);
        const get = getter(p);
        const nl = /\r\n/.test(text) ? '\r\n' : '\n';
        const replace = {}, remove = {}, append = {}; // line -> text, line -> true, section name -> [text]
        const add = (sec, line) => (append[sec] = append[sec] || []).push(line);
        const settle = (sec, key, val) => {
            const at = p.keys[id(sec, key)];
            if (val == null) { if (at) remove[at.line] = true; }
            else if (!at) add(sec, key + '=' + val);
            else if (at.value !== val) replace[at.line] = p.lines[at.line].replace(/=.*$/, '=' + val);
        };
        C.OPTIONS.forEach(o => {
            if (same(readOption(o, get), values[o.id])) return; // the file already says this: leave its lines alone
            const vals = writeOf(o, values[o.id]);
            keysOf(o).forEach(([sec, key], i) => settle(sec, key, vals[i]));
        });
        C.ALWAYS.forEach(([sec, key, val]) => settle(sec, key, val));

        const after = {}; // line -> [text] for sections that exist
        const newSections = [];
        Object.keys(append).forEach(sec => {
            const s = p.sections[sec.toLowerCase()];
            if (s) (after[s.last] = after[s.last] || []).push(...append[sec]);
            else newSections.push(sec);
        });
        const out = [];
        p.lines.forEach((line, i) => {
            if (!remove[i]) out.push(i in replace ? replace[i] : line);
            if (after[i]) out.push(...after[i]);
        });
        while (out.length && out[out.length - 1] === '') out.pop();
        newSections.sort((a, b) => SECTION_ORDER.indexOf(a) - SECTION_ORDER.indexOf(b)).forEach(sec => out.push('[' + sec + ']', ...append[sec]));
        return out.join(nl) + nl;
    }

    function fresh(C, values) {
        const w = wanted(C, values).filter(x => x.val != null);
        const lines = ['; CryMT settings, read at every start of Bin64\\Crysis.exe. Written by the CryMT launcher;',
                       '; lines and comments added by hand are kept when it saves.'];
        SECTION_ORDER.forEach(sec => {
            const mine = w.filter(x => x.sec === sec);
            if (mine.length) lines.push('[' + sec + ']', ...mine.map(x => x.key + '=' + x.val));
        });
        return lines.join('\r\n') + '\r\n';
    }

    root.CryIni = {parse, readValues, readOption, wanted, merge, fresh, keysOf, writeOf, valid, same};
})(typeof window !== 'undefined' ? window : globalThis);
