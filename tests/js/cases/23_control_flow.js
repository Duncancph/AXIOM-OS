function sw(x) { switch (x) { case 1: return 'one'; case '1': return 'string one'; case 2: case 3: return 'two-three'; default: return 'other'; case 4: return 'four'; } }
console.log([1, '1', 2, 3, 4, 5, null].map(sw));
function fall(x) { const r = []; switch (x) { case 0: r.push(0); case 1: r.push(1); break; case 2: r.push(2); default: r.push('d'); case 3: r.push(3); } return r.join(); }
console.log(fall(0), fall(1), fall(2), fall(3), fall(9));
outer: for (let i = 0; i < 4; i++) { inner: for (let j = 0; j < 4; j++) { if (j === 2) continue outer; if (i === 3) break outer; console.log('ij', i, j); } }
let k = 0; do { k++; if (k === 2) continue; console.log('do', k); } while (k < 4);
let w = 10; while (w --> 7) console.log('w', w);
for (let i = 0, j = 5; i < j; i += 2, j--) console.log('two vars', i, j);
for (;;) { console.log('infinite once'); break; }
const obj = { a: 1, b: 2 }; Object.defineProperty(obj, 'h', { value: 3, enumerable: false }); const proto = { inh: 4 }; Object.setPrototypeOf(obj, proto);
for (const key in obj) console.log('in', key);
for (const i in [7, 8]) console.log('arr in', i, typeof i);
for (const ch in 'ab') console.log('str in', ch);
const deleting = { x: 1, y: 2, z: 3 }; for (const kk in deleting) { console.log('del', kk); delete deleting.y; }
let r = ''; for (const c of 'héllo') r += c.toUpperCase(); console.log(r);
blk: { if (true) break blk; console.log('unreached'); }
console.log((() => { switch (true) { case 1 > 2: return 'a'; case 2 > 1: return 'b'; } })());
let n = 0; while (true) { if (++n > 5) break; } console.log(n);
for (var vi = 0; vi < 3; vi++) {} console.log(vi);
const tern = x => x > 0 ? 'pos' : x < 0 ? 'neg' : 'zero'; console.log(tern(1), tern(-1), tern(0));
console.log(true && 'a', false && 'a', null || 'b', 0 || '', '' ?? 'n', null ?? 'n', undefined ?? 0 ?? 1);
let evals = 0; const lhs = () => { evals++; return null; }; const res = lhs()?.prop?.deep; console.log(res, evals);
switch ('x') { default: console.log('default first'); case 'y': console.log('falls to y'); }
let sumOdd = 0; for (let i = 0; i < 20; i++) { if (i % 2 === 0) continue; if (i > 15) break; sumOdd += i; } console.log(sumOdd);
const it = [1, 2, 3]; for (const [i, v] of it.entries()) { if (i === 1) continue; console.log('entry', i, v); }
try { for (const x of 5) {} } catch (e) { console.log(e.name, e.message) }
try { for (const x of { a: 1 }) {} } catch (e) { console.log(e.name, e.message) }
if (0) console.log('no'); else if ('') console.log('no'); else console.log('else chain');
var asi = 1
var asi2 = 2
;[asi, asi2].forEach(v => console.log('asi', v))
const asiRet = (function () { return
  42 })(); console.log(asiRet)
let aa = 1, bb = 2
aa
++bb
console.log(aa, bb)
