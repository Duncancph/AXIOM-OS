function outer() { let n = 0; return { inc: () => ++n, get: () => n }; }
const c1 = outer(), c2 = outer(); c1.inc(); c1.inc(); c2.inc(); console.log(c1.get(), c2.get());
function defaults(a, b = a * 2, c = b + 1) { return [a, b, c]; } console.log(defaults(1), defaults(1, 5), defaults(1, undefined, 0), defaults.length);
function rest(a, ...r) { return [a, r, arguments.length]; } console.log(rest(1, 2, 3), rest(), rest.length);
function args() { return Array.prototype.slice.call(arguments).reverse(); } console.log(args(1, 2, 3), (function () { return arguments.length; })(1, 2));
function argsMut(a) { arguments[0] = 99; return a; } console.log(argsMut(1));
const obj = { name: 'obj', regular() { return this.name; }, arrow: () => typeof this, nested() { return [1].map(() => this.name); } };
console.log(obj.regular(), obj.arrow(), obj.nested(), (0, obj.regular)() === undefined);
function Sloppy() { return this === globalThis; } function Strict() { 'use strict'; return this; } console.log(Sloppy(), Strict(), Strict.call(5), typeof Sloppy.call(5));
function greet(g, p) { return g + ' ' + this.n + p; } const bound = greet.bind({ n: 'Ann' }, 'Hi'); console.log(bound('!'), greet.call({ n: 'Bo' }, 'Yo', '?'), greet.apply({ n: 'Cy' }, ['Hey', '.']), bound.name, bound.length);
function Point(x, y) { this.x = x; this.y = y; } Point.prototype.len = function () { return Math.hypot(this.x, this.y); };
const p = new Point(3, 4); console.log(p.len(), p instanceof Point, p.constructor === Point, Object.keys(p), typeof Point.prototype);
const BoundPoint = Point.bind(null, 1); const bp = new BoundPoint(2); console.log(bp.x, bp.y, bp instanceof Point);
console.log(hoisted(), typeof notYet); function hoisted() { return 'hoisted!'; } var notYet = 1;
var fe = function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }; console.log(fe(10), typeof fact);
const counterGen = (() => { let c = 0; return () => c++; })(); counterGen(); console.log(counterGen(), counterGen());
function curry(f) { return a => b => c => f(a, b, c); } console.log(curry((a, b, c) => a + b + c)(1)(2)(3));
const compose = (...fns) => x => fns.reduceRight((v, f) => f(v), x); console.log(compose(x => x + 1, x => x * 2)(5));
function memo(f) { const cache = new Map(); return n => cache.has(n) ? cache.get(n) : (cache.set(n, f(n)), cache.get(n)); }
const mfib = memo(n => n < 2 ? n : mfib(n - 1) + mfib(n - 2)); console.log(mfib(80));
console.log((function () { return typeof arguments; })(), (() => { try { return typeof arguments; } catch (e) { return 'err'; } })());
function recurse(n) { return n === 0 ? 0 : 1 + recurse(n - 1); } console.log(recurse(5000));
let depth = 0; function inf() { depth++; inf(); } try { inf(); } catch (e) { console.log(e instanceof RangeError, e.message, depth > 1000); }
console.log(new Function('a', 'b', 'return a * b')(6, 7), Function('return 1 + 1')(), (new Function('return this'))() === globalThis);
const f = function () {}; const af = () => {}; console.log(f.name, af.name, (function () {}).name, (() => {}).name, ({ m() {} }).m.name);
console.log(typeof f.prototype, af.prototype, Object.getOwnPropertyNames(af).sort());
try { new af() } catch (e) { console.log(e.name, e.message) }
try { (void 0)() } catch (e) { console.log(e.name, e.message) }
try { const q = {}; q.notfn() } catch (e) { console.log(e.name, e.message) }
try { const q = { a: { b: 1 } }; q.a.b() } catch (e) { console.log(e.name, e.message) }
try { const q = [1]; q[0]() } catch (e) { console.log(e.name, e.message) }
try { new (function () { return 1 }) } catch (e) { console.log(e.name) }
console.log(new (function () { return { z: 1 } })().z, new (function () { this.a = 1; return 5 })().a);
function nt() { return new.target === nt; } console.log(nt(), new nt() instanceof nt);
function selfRef() { return selfRef.toString().length > 0; } console.log(selfRef(), Function.prototype.toString.call(Math.max), (x => x * 2).toString(), (function (a, b) { return a + b; }).toString());
var x = 'global'; function scopeTest() { var x = 'local'; return (() => x)(); } console.log(scopeTest(), x);
const iife = (function (n) { return n * n; })(9); console.log(iife);
console.log([1, 2, 3].map(function (x) { return this.m * x; }, { m: 10 }));
function cbStyle(cb) { cb(null, 'done'); } cbStyle((err, v) => console.log(err, v));
