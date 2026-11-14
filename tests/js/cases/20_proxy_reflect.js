const target = { a: 1, b: 2 };
const log = [];
const p = new Proxy(target, {
  get(t, k, r) { log.push('get ' + String(k)); return k in t ? t[k] : 'default'; },
  set(t, k, v) { log.push('set ' + k); t[k] = v * 2; return true; },
  has(t, k) { log.push('has ' + k); return k !== 'hidden'; },
  deleteProperty(t, k) { log.push('delete ' + k); delete t[k]; return true; },
  ownKeys(t) { log.push('ownKeys'); return [...Reflect.ownKeys(t), 'virtual']; },
  getOwnPropertyDescriptor(t, k) { log.push('gopd ' + k); return k === 'virtual' ? { value: 'v', enumerable: true, configurable: true } : Reflect.getOwnPropertyDescriptor(t, k); },
});
console.log(p.a, p.zzz, 'a' in p, 'hidden' in p); p.c = 5; console.log(target.c, delete p.a, target.a, Object.keys(p), JSON.stringify(p));
console.log(log.join(' | '));
const fnProxy = new Proxy(function (a, b) { return a + b; }, { apply(t, thisArg, args) { return t(...args) * 10; }, construct(t, args) { return { constructed: args }; } });
console.log(fnProxy(1, 2), new fnProxy(1, 2).constructed, typeof fnProxy);
const { proxy, revoke } = Proxy.revocable({ x: 1 }, {}); console.log(proxy.x); revoke(); try { proxy.x } catch (e) { console.log(e.name, e.message) }
const validator = new Proxy({}, { set(t, k, v) { if (typeof v !== 'number') throw new TypeError(k + ' must be number'); t[k] = v; return true; } });
validator.age = 30; try { validator.age = 'x' } catch (e) { console.log(e.message) } console.log(validator.age);
const negArray = new Proxy([1, 2, 3], { get(t, k, r) { const i = Number(k); return Number.isInteger(i) && i < 0 ? t[t.length + i] : Reflect.get(t, k, r); } });
console.log(negArray[-1], negArray[0], negArray.length, Array.isArray(negArray), negArray.map(x => x * 2));
console.log(Reflect.has({ a: 1 }, 'a'), Reflect.get({ a: 1 }, 'a'), Reflect.ownKeys({ b: 1, [Symbol('s')]: 2 }).length, Reflect.getPrototypeOf([]) === Array.prototype);
const ro = {}; Reflect.defineProperty(ro, 'x', { value: 1 }); console.log(Reflect.set(ro, 'x', 2), ro.x, Reflect.deleteProperty(ro, 'x'), Reflect.isExtensible(ro), Reflect.preventExtensions(ro), Reflect.isExtensible(ro));
console.log(Reflect.apply(Math.max, null, [1, 5, 2]), Reflect.construct(Date, [0]).getTime(), JSON.stringify(Reflect.getOwnPropertyDescriptor({ q: 1 }, 'q')));
class A { constructor() { this.nt = new.target.name; } } class B {} console.log(Reflect.construct(A, [], B).nt, Reflect.construct(A, [], B) instanceof B);
const getterRecv = { get g() { return this.v; }, v: 'orig' }; console.log(Reflect.get(getterRecv, 'g', { v: 'recv' }));
const tracer = new Proxy({}, { get: (t, k) => typeof k === 'string' ? `<${k}>` : undefined }); console.log(tracer.foo, tracer.bar + '!');
const protoProxy = Object.create(new Proxy({}, { get: (t, k) => k === 'magic' ? 42 : undefined })); console.log(protoProxy.magic);
console.log(Object.prototype.toString.call(new Proxy([], {})), Object.prototype.toString.call(new Proxy(function () {}, {})));
