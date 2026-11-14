const name = 'World', n = 3;
console.log(`Hello ${name}!`, `${n} + ${n} = ${n + n}`, `nested ${`inner ${n * 2}`} end`, `multi
line`, `\${escaped}`, `${[1, 2]}`, `${{}}`, `${null} ${undefined}`, `a${''}b`);
function tag(strings, ...vals) { return strings.raw.map((s, i) => s + (i < vals.length ? '[' + vals[i] + ']' : '')).join('') + '|' + strings.length + '|' + Object.isFrozen(strings); }
console.log(tag`a${1}b${2}c`, tag`\n${'x'}`, tag``, String.raw`C:\path\${name}`);
function cache(s) { return s; } const t1 = cache`x`, t2 = cache`x`; const ts = []; for (let i = 0; i < 2; i++) ts.push(cache`y`); console.log(t1 === t2, ts[0] === ts[1]);
function badEsc(s) { return s[0] === undefined && s.raw[0] === '\\unicode'; } console.log(badEsc`\unicode`);
const o = { a: { b: { c: 1, f() { return 'called'; } } }, arr: [1, { x: 'deep' }], nul: null };
console.log(o?.a?.b?.c, o.x?.y, o.x?.y.z.w, o.a?.['b']?.c, o.arr?.[1]?.x, o.nul?.x, o.a.b.f?.(), o.a.b.g?.(), o.nope?.(), o.a?.b.f());
console.log(o.nul ?? 'default', o.a.b.c ?? 'unused', 0 ?? 'zero kept', '' ?? 'empty kept', false ?? 'f');
let calls = 0; const getter = { get v() { calls++; return null; } }; getter.v?.x; getter.v ?? 1; console.log(calls);
const fnNull = null; console.log(fnNull?.(), typeof fnNull?.call);
console.log(delete o?.a?.b?.c, o.a.b.c, delete o.nul?.x);
const deep = { l1: { l2: { l3: { l4: 'bottom' } } } }; const path = ['l1', 'l2', 'l3', 'l4']; console.log(path.reduce((acc, k) => acc?.[k], deep), ['l1', 'zz', 'l3'].reduce((acc, k) => acc?.[k], deep));
const arr = null; console.log(arr?.[0], arr?.length, (arr ?? [9])[0]);
class C { #p = 'priv'; static get(o) { return o?.#p; } } console.log(C.get(new C()), C.get(null));
const html = (strings, ...values) => strings.reduce((out, s, i) => out + s + (i < values.length ? String(values[i]).replace(/</g, '&lt;') : ''), '');
console.log(html`<div>${'<script>'}</div>`);
console.log(`${1 + 1}${'2'}${[3]}`, `line1\nline2`.split('\n').length, `\u0041\x42\u{43}`, `tab\there`);
