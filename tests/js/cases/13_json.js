const obj = { s: 'str', n: 1.5, b: true, z: null, u: undefined, f() {}, sym: Symbol('x'), arr: [1, 'two', null, undefined, () => 0], nested: { deep: { er: [] } }, date: new Date(0) };
console.log(JSON.stringify(obj)); console.log(JSON.stringify(obj, null, 2)); console.log(JSON.stringify(obj, null, '--'));
console.log(JSON.stringify([1, [2, [3]]], null, 1), JSON.stringify({}, null, 2), JSON.stringify([], null, 2), JSON.stringify({ a: [] }, null, 2));
console.log(JSON.stringify(obj, ['s', 'n', 'nested', 'deep']), JSON.stringify(obj, (k, v) => typeof v === 'number' ? v * 2 : v));
console.log(JSON.stringify('he said "hi"\n\t\u0001\u001f\u007f 😀\ud800'), JSON.stringify(NaN), JSON.stringify(-0), JSON.stringify(Infinity), JSON.stringify(1e21), JSON.stringify(undefined), JSON.stringify(() => 1), JSON.stringify(Symbol()));
console.log(JSON.stringify({ toJSON() { return { replaced: 1 }; } }), JSON.stringify({ d: new Date(Date.UTC(2020, 0, 2, 3, 4, 5, 6)) }), JSON.stringify([new Number(3), new String('s'), new Boolean(false)]));
const circ = { a: 1 }; circ.self = circ; try { JSON.stringify(circ) } catch (e) { console.log(e.name, e.message.split('\n')[0]) }
try { JSON.stringify({ b: 1n }) } catch (e) { console.log(e.name, e.message) }
const parsed = JSON.parse('{"a":[1,2,{"b":null}],"c":"\\u0041\\n","d":-1.5e2,"e":true,"__proto__":{"x":1}}');
console.log(parsed.a[2].b, parsed.c, parsed.d, parsed.e, Object.keys(parsed), JSON.stringify(parsed));
console.log(JSON.parse('[1, "2", {"3": [4]}]', (k, v) => typeof v === 'number' ? v + 100 : v), JSON.parse(' "x" '), JSON.parse('null'), JSON.parse('12.5e-1'));
const reviveOrder = []; JSON.parse('{"a":{"b":1},"c":[2]}', function (k, v) { reviveOrder.push(k || '(root)'); return v; }); console.log(reviveOrder.join(','));
for (const bad of ['', '{', '[1,]', '{"a":1,}', 'tru', '01', '1.', '-', '"abc', '{a:1}', "'x'", '[1 2]', '{"a" 1}', 'nul', '"\\x"', '1 2', '{"a":1}x', '"\t"']) {
  try { JSON.parse(bad); console.log('parsed?', bad) } catch (e) { console.log(JSON.stringify(bad), e.name, e.message) }
}
console.log(JSON.stringify({ k: 'v' }, null, 20).split('\n')[1].length, JSON.stringify([1, 2], null, '\t'), JSON.stringify({ a: 1, b: 2 }, ['b']));
const big = { list: Array.from({ length: 5 }, (_, i) => ({ id: i, name: 'item' + i, tags: i % 2 ? ['odd'] : [] })) };
const round = JSON.parse(JSON.stringify(big)); console.log(round.list.length, round.list[3].name, round.list[3].tags[0], JSON.stringify(round) === JSON.stringify(big));
console.log(JSON.stringify(Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true } })), JSON.stringify([,1]), JSON.stringify({ [Symbol('s')]: 1, n: 2 }));
console.log(JSON.stringify(new Map([[1, 2]])), JSON.stringify(new Set([1])), JSON.stringify(/re/), JSON.stringify(new Error('e')), JSON.stringify(Object.assign([1, 2], { extra: 3 })));
console.log(JSON.stringify('é中'), JSON.parse('"\\u00e9\\ud83d\\ude00"'), JSON.stringify({ '\n': 1 }), JSON[Symbol.toStringTag]);
console.log(JSON.stringify(1e-7), JSON.stringify(123456789012345680000), JSON.stringify([0.1 + 0.2]), JSON.parse('1E400'), JSON.parse('-0'), Object.is(JSON.parse('-0'), -0));
