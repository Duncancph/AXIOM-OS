const { a, b: { c, d = 4 } = {}, e = 'def', ...others } = { a: 1, b: { c: 3 }, f: 6, g: 7 };
console.log(a, c, d, e, others);
const [x, [y, z = 'z'], ...tail] = [1, [2], 3, 4]; console.log(x, y, z, tail);
let m = 1, n = 2; [m, n] = [n, m]; console.log(m, n);
const { length } = 'hello'; const [first, ...chars] = 'hey'; console.log(length, first, chars);
function f({ name = 'anon', tags: [t1 = 'none'] = [] } = {}, ...[r1, r2]) { return [name, t1, r1, r2]; }
console.log(f(), f({ name: 'n', tags: ['x'] }, 1, 2));
for (const [k, v] of Object.entries({ p: 1, q: 2 })) console.log(k, v);
for (const { id, ...rest } of [{ id: 1, v: 'a' }, { id: 2, w: 'b' }]) console.log(id, JSON.stringify(rest));
const key = 'dyn'; const { [key]: dynVal = 'dflt', [`${key}2`]: dyn2 } = { dyn2: 2 }; console.log(dynVal, dyn2);
const o = {}; ({ a: o.x, b: o['y'] } = { a: 10, b: 20 }); [o.z] = [30]; console.log(o);
console.log(Math.max(...[1, 5, 3]), [...[1, 2], ...'ab', ...new Set([3])], { ...{ a: 1 }, ...{ a: 2, b: 3 } });
function sum(...nums) { return nums.reduce((s, x) => s + x, 0); } console.log(sum(...[1, 2], 3, ...[4]));
const arr = [1, 2, 3]; const copy = [...arr]; copy.push(4); console.log(arr, copy);
try { const { q } = null; } catch (e) { console.log(e.name, e.message) }
try { const [q] = {}; } catch (e) { console.log(e.name, e.message) }
try { const [q] = undefined; } catch (e) { console.log(e.name) }
const { 0: zero, 1: one } = ['zero', 'one']; console.log(zero, one);
const [, , third = 'missing'] = [1, 2]; console.log(third);
let it = 0; const iterable = { *[Symbol.iterator]() { yield 1; it++; yield 2; it++; yield 3; } }; const [i1, i2] = iterable; console.log(i1, i2, it);
const nested = { a: [{ b: { c: [1, 2, { d: 'deep' }] } }] }; const { a: [{ b: { c: [, , { d: deep }] } }] } = nested; console.log(deep);
const fnDefault = ({ x = 1, y = x * 2 } = {}) => x + y; console.log(fnDefault(), fnDefault({ x: 5 }), fnDefault({ y: 1 }));
const swap = ([p, q]) => [q, p]; console.log(swap([1, 2]));
let count = 0; const { [(count++, 'a')]: v1 = count++ } = {}; console.log(v1, count);
const arrLike = { length: 2, 0: 'a', 1: 'b' }; console.log(Array.from(arrLike), [...Array.from(arrLike).entries()]);
