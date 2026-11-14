// mapped arguments, error message details, misc edge semantics
function mapped(a, b) { arguments[0] = 'A'; b = 'B'; return [a, arguments[1], arguments.length]; }
console.log(mapped(1, 2), mapped(1));
function unmapped(a) { 'use strict'; arguments[0] = 9; return a; }
console.log(unmapped(1));
function delMap(a) { delete arguments[0]; arguments[0] = 5; return [a, arguments[0]]; }
console.log(delMap(1));
function defMap(a) { Object.defineProperty(arguments, '0', { value: 7 }); const r1 = a; a = 8; return [r1, arguments[0]]; }
console.log(defMap(1));
function argsInspect() { return arguments; }
console.log(argsInspect(1, 'x'), Object.prototype.toString.call(argsInspect()));
function withDefaults(a = 1) { arguments[0] = 99; return a; }
console.log(withDefaults(5));
const cases = [
  'for (const x of 5){}', 'for (const x of {a:1}){}', 'const o={}; for (const x of o){}', 'const [q] = {}', 'const [q] = 5', '[...5]',
  'Math.max(...5)', 'Math.max(...{})', 'new Map(5)', 'const {q} = null', 'const {} = undefined', 'const {a:{b}} = {}',
  'for (const x of undefined){}', 'new Set(5)', 'let a; [a] = null', 'function f({a}){}; f()', 'let a; ({a} = null)',
  'const [{a}] = [null]', 'const o2=null; const {x} = o2', 'const {x} = g(); function g(){return null}', 'const s=[...null]',
  'const s=[...[1,2].x]', 'for (const x of null){}', 'const [a] = true', 'const [a] = -1', '(void 0)()', 'null.x', 'undefined[0]',
  'class A { #m(){} static t(o){ o.#m(); } }; A.t({})', 'class A { #m(){} static t(o){ o.#m = 1; } }; A.t(new A)',
  'class A { #f=1; static t(o){ return o.#f; } }; A.t({})', 'class A{ static #s(){} static t(o){o.#s()} }; A.t({})',
  '(class { #m(){} static t(o){ o.#m(); } }).t({})', 'class A { set #s(v){} static t(o){ return o.#s; } }; A.t(new A)',
];
for (const c of cases) { try { eval(c); console.log(c, 'OK'); } catch (e) { console.log(c, '=>', e.name, e.message); } }
for (let i = 1; i < 40; i++) console.log(Math.pow(1.1, i), 3.7417215084677413 ** (i / 2), Math.pow(i, -1 / 3));
for (const t of ['Café', 'Å', 'Ångström', 'Tiếng Việt', 'Ωμέγα', 'Йод', 'plain'])
  console.log(t.normalize('NFD').length, t.normalize().length, t.normalize('NFD').normalize('NFC') === t.normalize(), [...t.normalize('NFD')].map(c => c.codePointAt(0).toString(16)).join(' '));
console.log(Object.getOwnPropertyNames(console).includes('%counts'), typeof console.log);
