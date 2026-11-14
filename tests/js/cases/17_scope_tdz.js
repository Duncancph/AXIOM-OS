try { console.log(tdz); let tdz = 1; } catch (e) { console.log(e.name, e.message) }
try { typeof tdz2; let tdz2; } catch (e) { console.log(e.name) }
{ let blockScoped = 'inner'; var fnScoped = 'var'; } console.log(typeof blockScoped, fnScoped);
const fns = []; for (var i = 0; i < 3; i++) fns.push(() => i); console.log(fns.map(f => f()));
const fns2 = []; for (let j = 0; j < 3; j++) { fns2.push(() => j); } console.log(fns2.map(f => f()));
const fns3 = []; for (const k of ['a', 'b']) fns3.push(() => k); console.log(fns3.map(f => f()));
const fns4 = []; for (const k in { x: 1, y: 2 }) fns4.push(() => k); console.log(fns4.map(f => f()));
let shadow = 'outer'; { let shadow = 'inner'; console.log(shadow); } console.log(shadow);
function hoistTest() { console.log(typeof inner, v); var v = 1; function inner() {} } hoistTest();
console.log(typeof undeclaredThing, typeof Math, typeof globalThis.Math);
var gv = 'gvar'; let gl = 'glet'; console.log(globalThis.gv, globalThis.gl, 'gv' in globalThis);
implicitGlobal = 5; console.log(globalThis.implicitGlobal, delete globalThis.implicitGlobal, typeof implicitGlobal);
(function () { 'use strict'; try { strictUndeclared = 1 } catch (e) { console.log(e.name, e.message) } })();
switch (1) { case 1: { let sw = 'case1'; console.log(sw); } }
try { eval('const noInit;') } catch (e) { console.log(e.name, e.message) }
const counter = (() => { let count = 0; return { inc() { return ++count; } }; })(); counter.inc(); console.log(counter.inc());
function outer() { let x = 1; function mid() { let y = 2; function inner() { return x + y; } x = 10; return inner; } return mid(); } console.log(outer()());
let loopClosures = []; for (let i = 0, j = 10; i < 3; i++, j--) loopClosures.push(() => i + ':' + j); console.log(loopClosures.map(f => f()));
let w = 0; const whileFns = []; while (w < 3) { let c = w; whileFns.push(() => c); w++; } console.log(whileFns.map(f => f()));
try { (() => { inner2(); const x = 1; function inner2() { return x; } })(); } catch (e) { console.log(e.name, e.message) }
if (true) { function blockFn() { return 'block fn'; } } console.log(typeof blockFn, blockFn());
console.log((function () { if (false) { var neverSet = 1; } return neverSet; })());
let a1 = 1; { let a1 = 2; { let a1 = 3; console.log(a1); } console.log(a1); } console.log(a1);
const obj = { val: 1, getVal() { const val = 2; return [val, this.val]; } }; console.log(obj.getVal());
try { (function () { 'use strict'; delete Object.prototype; })() } catch (e) { console.log(e.name, e.message) }
var x = 1; { var x = 2; } console.log(x);
function paramScope(a, b = () => a) { var a = 5; return b(); } console.log(paramScope(1));
console.log(typeof eval, eval('1 + 1'), eval('var evalVar = 3; evalVar * 2'), typeof evalVar);
console.log((0, eval)('typeof globalThis'), eval('(function(){ return this })()') === globalThis);
const cl = class { static m() { return typeof cl; } }; console.log(cl.m());
let lateInit; console.log(lateInit); lateInit = 'set'; console.log(lateInit);
label1: { console.log('in block'); break label1; console.log('never'); } console.log('after block');
