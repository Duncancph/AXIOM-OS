function* range(s, e, step = 1) { for (let i = s; i < e; i += step) yield i; }
console.log([...range(0, 5)], [...range(0, 10, 3)], Array.from(range(1, 4), x => x * x));
function* gen() { const x = yield 1; console.log('got', x); try { yield 2; } finally { console.log('cleanup'); } yield 3; return 4; }
const g = gen(); console.log(g.next('ignored'), g.next('A'), g.return(9), g.next());
const g2 = gen(); g2.next(); g2.next(); try { g2.throw(new Error('thrown')) } catch (e) { console.log('caught', e.message) } console.log(g2.next());
function* delegating() { yield 'a'; const r = yield* inner(); yield r; yield* [1, 2]; yield* 'xy'; }
function* inner() { yield 'i1'; yield 'i2'; return 'inner-ret'; }
console.log([...delegating()]);
const iterObj = { from: 1, to: 4, [Symbol.iterator]() { let c = this.from, t = this.to; return { next: () => c <= t ? { value: c++, done: false } : { value: undefined, done: true }, [Symbol.iterator]() { return this; } }; } };
console.log([...iterObj], Math.max(...iterObj));
function* fibs() { let [a, b] = [0, 1]; for (;;) { yield a; [a, b] = [b, a + b]; } }
const out = []; for (const f of fibs()) { if (f > 100) break; out.push(f); } console.log(out.join(','));
let closed = false; const closer = { [Symbol.iterator]() { return { i: 0, next() { return { value: this.i++, done: false }; }, return() { closed = true; return { done: true }; } }; } };
for (const v of closer) { if (v === 2) break; } console.log('closed', closed);
closed = false; try { for (const v of closer) { throw new Error('x'); } } catch (e) { console.log('closed on throw', closed); }
const genObj = (function* () {})(); console.log(typeof genObj.next, genObj[Symbol.iterator]() === genObj, Object.prototype.toString.call(genObj), String(genObj));
function* argsGen() { yield* arguments; } console.log([...argsGen(5, 6)]);
const entries = new Map([['a', 1], ['b', 2]]).entries(); console.log(entries.next(), [...entries]);
const ai = [1, 2, 3][Symbol.iterator](); ai.next(); console.log([...ai], Object.prototype.toString.call(ai));
function* withReturn() { try { yield 1; yield 2; } finally { yield 'from finally'; } }
const wr = withReturn(); console.log(wr.next(), wr.return('R'), wr.next(), wr.next());
function* yieldStarThrow() { try { yield* (function* () { try { yield 1; } catch (e) { yield 'inner caught ' + e; } })(); } catch (e) { yield 'outer ' + e; } }
const yst = yieldStarThrow(); console.log(yst.next(), yst.throw('E'), yst.next());
const lazyMap = function* (it, f) { for (const x of it) yield f(x); }; console.log([...lazyMap(range(0, 4), x => x * 10)]);
function* inf() { let i = 0; while (true) yield i++; } const [p, q, r] = inf(); console.log(p, q, r);
const gs = function* () { yield this.v; }; console.log([...gs.call({ v: 'thisv' })]);
try { const g3 = (function* () { yield g3.next(); })(); g3.next(); } catch (e) { console.log(e.name, e.message) }
console.log(Object.getPrototypeOf(function* () {}).constructor.name, typeof (function* () {}).prototype);
const sIter = 'ab'[Symbol.iterator](); console.log(sIter.next(), sIter.next(), sIter.next(), String(sIter));
