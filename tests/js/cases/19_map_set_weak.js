const m = new Map([['a', 1], ['b', 2]]); m.set('c', 3).set(NaN, 'nan').set(0, 'zero'); m.set(-0, 'negzero');
console.log(m.size, m.get('a'), m.get(NaN), m.get(0), m.has('b'), m.has('z'), m.delete('b'), m.delete('b'), m.size, [...m.keys()], [...m.values()], [...m]);
const ko = {}; m.set(ko, 'obj'); console.log(m.get(ko), m.get({}), [...m.entries()].length);
m.forEach((v, k, map) => console.log('each', String(k), v, map === m));
const s = new Set([1, 2, 2, 3, '3', NaN, NaN]); console.log(s.size, s.has('3'), s.has(NaN), [...s], s.delete(1), [...s.entries()].slice(0, 2), [...s.keys()]);
const sm = new Set([1, 2, 3, 4]); for (const x of sm) { if (x === 2) sm.delete(3); if (x === 4) sm.add(5); console.log('iter', x); }
const order = new Map(); order.set('z', 1); order.set('a', 2); order.delete('z'); order.set('z', 3); console.log([...order.keys()]);
const big = new Map(); for (let i = 0; i < 5000; i++) big.set('k' + i, i); for (let i = 0; i < 5000; i += 2) big.delete('k' + i); console.log(big.size, big.get('k4999'), big.get('k4998'), [...big.keys()].slice(0, 3));
const a = new Set([1, 2, 3]), b = new Set([2, 3, 4]);
console.log([...a.union(b)], [...a.intersection(b)], [...a.difference(b)], [...a.symmetricDifference(b)], a.isSubsetOf(new Set([1, 2, 3, 4])), a.isSupersetOf(new Set([1])), a.isDisjointFrom(new Set([9])));
const wm = new WeakMap(); const k1 = {}, k2 = function () {}; wm.set(k1, 'one').set(k2, 'two');
console.log(wm.get(k1), wm.get(k2), wm.has(k1), wm.has({}), wm.delete(k1), wm.has(k1), wm.get(k1));
try { wm.set('str', 1) } catch (e) { console.log(e.name, e.message) }
const ws = new WeakSet([k1]); ws.add(k2); console.log(ws.has(k1), ws.has(k2), ws.has({}), ws.delete(k2), ws.has(k2));
const frozenKey = Object.freeze({}); wm.set(frozenKey, 'frozen ok'); console.log(wm.get(frozenKey), Object.keys(frozenKey).length, Object.getOwnPropertySymbols(frozenKey).length);
const wr = new WeakRef(k1); console.log(wr.deref() === k1, Object.prototype.toString.call(wr));
const fr = new FinalizationRegistry(() => {}); fr.register(k1, 'held'); console.log(typeof fr.unregister);
console.log(String(m), Object.prototype.toString.call(s), typeof Map.prototype.entries, Map.prototype[Symbol.iterator] === Map.prototype.entries, Set.prototype.keys === Set.prototype.values);
const mi = new Map([[1, 'a']])[Symbol.iterator](); console.log(mi.next(), mi.next(), String(mi));
console.log(JSON.stringify([...new Map([[{ x: 1 }, [1, 2]]])]), new Map([[1, 2]]).size, new Set('hello').size, [...new Set('hello')].join(''));
const grouped = Map.groupBy([1, 2, 3, 4, 5], x => x % 2 ? 'odd' : 'even'); console.log([...grouped.keys()], grouped.get('odd'));
try { new Map([1]) } catch (e) { console.log(e.name, e.message) }
try { Map() } catch (e) { console.log(e.name, e.message) }
const cleared = new Map([[1, 1]]); cleared.clear(); console.log(cleared.size, [...cleared]);
class MyMap extends Map { getOr(k, d) { return this.has(k) ? this.get(k) : d; } } const mm = new MyMap([['x', 1]]); console.log(mm.getOr('x', 0), mm.getOr('y', 0), mm instanceof Map);
