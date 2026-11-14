const log = [];
const p = (s) => log.push(s);
setTimeoutFake = null;
Promise.resolve().then(() => p('t1')).then(() => p('t2')).then(() => p('t3'));
(async () => { p('a1'); await null; p('a2'); await undefined; p('a3'); })();
Promise.resolve().then(() => p('u1')).then(() => p('u2'));
queueMicrotask(() => p('qm'));
new Promise(r => { p('exec'); r('val'); }).then(v => p('then:' + v));
const thenable = { then(res) { p('thenable.then'); res('tv'); } };
Promise.resolve(thenable).then(v => p('thenable resolved ' + v));
(async () => { const v = await thenable; p('await thenable ' + v); })();
(async () => { const r = await Promise.resolve('pr'); p('await promise ' + r); })();
async function ret() { return Promise.resolve('nested'); }
ret().then(v => p('ret ' + v));
p('sync');
Promise.reject(new Error('rej')).catch(e => p('caught ' + e.message)).finally(() => p('finally'));
Promise.all([1, Promise.resolve(2), new Promise(r => r(3))]).then(v => p('all ' + v));
Promise.allSettled([Promise.reject('x'), 1]).then(v => p('settled ' + JSON.stringify(v)));
Promise.race([new Promise(() => {}), Promise.resolve('fast')]).then(v => p('race ' + v));
Promise.any([Promise.reject(1), Promise.resolve('any-ok')]).then(v => p('any ' + v));
Promise.any([Promise.reject(1), Promise.reject(2)]).catch(e => p('anyfail ' + e.constructor.name + ' ' + e.errors + ' ' + e.message));
Promise.all([]).then(v => p('empty all ' + v.length));
async function thrower() { await 1; throw new TypeError('async boom'); }
thrower().catch(e => p('async caught ' + e.name));
async function tryCatch() { try { await Promise.reject('E1'); } catch (e) { return 'recovered ' + e; } finally { p('tc finally'); } }
tryCatch().then(v => p(v));
const { promise, resolve } = Promise.withResolvers(); promise.then(v => p('withResolvers ' + v)); resolve('wr');
Promise.resolve(1).then(2).then(v => p('passthrough ' + v));
Promise.reject(3).then(null, e => p('onRejected ' + e));
Promise.resolve(5).finally(() => 'ignored').then(v => p('finally keeps ' + v));
Promise.reject(6).finally(() => { throw 'fin-err' }).catch(e => p('finally throws ' + e));
async function seq() { let s = 0; for (const x of [1, 2, 3]) s += await x; return s; }
seq().then(v => p('seq ' + v));
async function par() { const [a, b] = await Promise.all([delay(2), delay(3)]); return a + b; }
function delay(v) { return new Promise(r => queueMicrotask(() => r(v))); }
par().then(v => p('par ' + v));
const aobj = { v: 7, async m() { return this.v; }, am: async () => 'arrow' };
aobj.m().then(v => p('method ' + v)); aobj.am().then(v => p(v));
class AC { async go() { await null; return 'class async'; } } new AC().go().then(p);
const chain = Promise.resolve(1).then(x => x + 1).then(x => { throw x * 10 }).catch(x => x + 1).then(x => p('chain ' + x));
p(typeof Promise.prototype.then + ' ' + Object.prototype.toString.call(Promise.resolve()));
new Promise((res, rej) => { res(1); rej(2); res(3); }).then(v => p('first wins ' + v));
new Promise(() => { throw new Error('exec throw'); }).catch(e => p('exec ' + e.message));
const cyc = new Promise(r => setTimeoutLike(() => r(cyc)));
function setTimeoutLike(f) { queueMicrotask(f); }
cyc.catch(e => p('cycle ' + e.constructor.name + ': ' + e.message));
async function* agen() { yield 1; await null; yield 2; }
(async () => { const out = []; for await (const v of agen()) out.push(v); for await (const w of [Promise.resolve('x'), 'y']) out.push(w); p('for await ' + out); })();
(async () => { try { await Promise.reject(new Error('aw')); } catch (e) { p('await reject ' + e.message); } })();
Promise.resolve().then(() => { console.log(log.join('\n')); });
setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => setTimeoutLike(() => console.log('---late\n' + log.slice().sort().join(',')))))))))))));
