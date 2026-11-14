function depth(n) { return n === 0 ? 0 : 1 + depth(n - 1); } console.log(depth(5000));
function sumTo(n) { if (n === 0) return 0; return n + sumTo(n - 1); } console.log(sumTo(3000));
let max = 0; function probe(n) { max = n; probe(n + 1); } try { probe(0); } catch (e) { console.log(e.name, e.message, max > 3000); }
const viaMap = n => n === 0 ? 0 : [n].map(x => viaMap(x - 1))[0] + 1; console.log(viaMap(300));
const viaGetter = { get deep() { return this.n-- > 0 ? this.deep + 1 : 0; }, n: 300 }; console.log(viaGetter.deep);
function viaToString(n) { return n === 0 ? '' : String({ toString: () => viaToString(n - 1) + 'x' }); } console.log(viaToString(200).length);
let nested = []; for (let i = 0; i < 1000; i++) nested = [nested]; console.log(JSON.stringify(nested).length);
try { let deepObj = {}; let cur = deepObj; for (let i = 0; i < 100000; i++) { cur.n = {}; cur = cur.n; } JSON.stringify(deepObj); console.log('stringified'); } catch (e) { console.log(e.name, e.message) }
try { eval('('.repeat(10000) + '1' + ')'.repeat(10000)); console.log('parsed deep'); } catch (e) { console.log(e.name) }
function* deepGen(n) { if (n > 0) yield* deepGen(n - 1); yield n; } let c = 0; for (const v of deepGen(200)) c++; console.log(c);
const mutual = { even: n => n === 0 ? true : mutual.odd(n - 1), odd: n => n === 0 ? false : mutual.even(n - 1) }; console.log(mutual.even(4000), mutual.odd(3001));
try { const a = []; a.push(a); String(a); console.log('cyclic join ok', String([1, [2, [3]]])); } catch (e) { console.log(e.name) }
async function adeep(n) { return n === 0 ? 0 : 1 + await adeep(n - 1); } adeep(500).then(v => console.log('async depth', v));
