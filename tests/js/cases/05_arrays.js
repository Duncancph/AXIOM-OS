const a = [5, 1, 4, 2, 3];
console.log(a.length, a.indexOf(4), a.includes(2), a.lastIndexOf(9), a.at(-2), a.join('-'), a.toString(), String(a));
console.log(a.slice(1, 3), a.slice(-2), a.concat([6, 7], 8, [[9]]), a.map(x => x * 2), a.filter(x => x % 2), a.reduce((s, x) => s + x, 0), a.reduceRight((s, x) => s + x, ''));
console.log(a.find(x => x > 3), a.findIndex(x => x > 3), a.findLast(x => x > 3), a.findLastIndex(x => x > 10), a.some(x => x > 4), a.every(x => x > 0));
const b = a.slice(); b.sort(); console.log(b, a.toSorted((x, y) => y - x), a.toReversed(), a.with(0, 99), a);
const c = [1, 2, 3, 4, 5, 6]; console.log(c.splice(1, 2), c, c.splice(-2, 1, 'x', 'y'), c, c.splice(1), c);
const d = [1, 2, 3]; console.log(d.push(4, 5), d.pop(), d.shift(), d.unshift(0, -1), d, d.reverse(), d.fill(7, 1, 3), d.copyWithin(0, 2));
console.log([1, [2, [3, [4]]]].flat(), [1, [2, [3, [4]]]].flat(Infinity), [1, 2].flatMap(x => [x, x * 10]), [[1, 2], [3]].flat().length);
console.log(Array.from('abc'), Array.from({ length: 3 }), Array.from([1, 2], x => x + 1), Array.of(7), Array(3), Array(3).length, new Array(2, 3), Array.isArray([]), Array.isArray({ length: 0 }));
console.log([..."hi"], [...new Set([1, 1, 2])], [1, 2, 3].keys().next(), [...[7, 8].entries()], [...['a', 'b'].keys()], [...['a'].values()]);
const holes = [1, , 3]; console.log(holes, holes.length, 1 in holes, holes.map(x => x * 2), holes.forEach(x => console.log('each', x)), holes.indexOf(undefined), holes.includes(undefined));
const sparse = []; sparse[5] = 'x'; console.log(sparse.length, sparse, Object.keys(sparse), sparse.join('|'));
const arr = [3, 1, 2]; arr.length = 1; console.log(arr, arr.length); arr.length = 3; console.log(arr);
console.log([10, 9, 1, 100, 25].sort(), [10, 9, 1, 100, 25].sort((x, y) => x - y), ['b', undefined, 'a', , 'c'].sort(), [true, false, 'x', 1].sort());
console.log([1, 2, 3].includes(NaN), [NaN].includes(NaN), [NaN].indexOf(NaN), [-0].includes(0), [1, 2, 3].indexOf(3, -1), [1, 2, 1].lastIndexOf(1, -2));
const people = [{ n: 'a', age: 30 }, { n: 'b', age: 25 }, { n: 'c', age: 30 }, { n: 'd', age: 25 }];
console.log(people.sort((x, y) => x.age - y.age).map(p => p.n).join(''), JSON.stringify(Object.groupBy(people, p => p.age)));
console.log([1, 2, 3].reduce((acc, x) => acc.concat([x, x]), []), [[1, 'a'], [2, 'b']].map(([n, s]) => s + n));
try { [].reduce((a, b) => a) } catch (e) { console.log(e.name, e.message) }
try { new Array(-1) } catch (e) { console.log(e.name, e.message) }
try { [].length = 1.5 } catch (e) { console.log(e.name, e.message) }
console.log([1, 2, 3].join(), [null, undefined, 1].join(), [[1, 2], [3]].join(';'), [1, 2, 3].toString(), [].toString() === '');
const nested = [1, [2, 3]]; nested.push(nested); console.log(nested.join(','), String([1, [2, [3, [4]]]]));
console.log(Array.prototype.slice.call({ 0: 'a', 1: 'b', length: 2 }), Array.prototype.map.call('ab', c => c + c), [].concat.call([1], 2));
const big = []; for (let i = 0; i < 10000; i++) big.push(i * 2 % 97); console.log(big.length, big.indexOf(50), big.reduce((s, x) => s + x), big.sort((x, y) => x - y)[5000], new Set(big).size);
console.log([3, 2, 1].map(String), ['1', '2', '3'].map(Number), ['10', '10', '10'].map(parseInt), [0, 1, '', null, 'a'].filter(Boolean));
const m = [[1, 2], [3, 4]]; console.log(m.map(r => r.map(x => x * x)), m[1][0], m.flat().reverse());
console.log(Array.from({ length: 5 }, (_, i) => i).filter(i => i & 1).map(i => i * i), [...Array(4).keys()]);
let cnt = 0; [1, 2, 3].forEach(function () { cnt += this.inc }, { inc: 5 }); console.log(cnt);
console.log([1, 2, 3, 4].findLast(x => x % 2 == 1), Array.prototype.concat.length, [].push.length, Array.length);
