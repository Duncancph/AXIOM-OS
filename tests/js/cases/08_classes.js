class Animal {
  static count = 0;
  #secret = 'hidden';
  name;
  type = 'animal';
  constructor(name) { this.name = name; Animal.count++; }
  speak() { return `${this.name} makes a sound`; }
  get secret() { return this.#secret; }
  set secret(v) { this.#secret = v; }
  #privMethod() { return 'pm:' + this.name; }
  callPriv() { return this.#privMethod(); }
  static create(n) { return new this(n); }
  static #sp = 's';
  static getSp() { return Animal.#sp; }
  hasSecret(o) { return #secret in o; }
  toString() { return `Animal(${this.name})`; }
}
class Dog extends Animal {
  constructor(name, breed) { super(name); this.breed = breed; }
  speak() { return super.speak() + ' (woof)'; }
  get desc() { return `${this.name} the ${this.breed}`; }
  static create(n) { return super.create(n + '!'); }
}
const d = new Dog('Rex', 'lab'); const a = new Animal('Cat');
console.log(d.speak(), a.speak(), d.desc, d.secret, (d.secret = 'x', d.secret), d.callPriv(), Animal.count, d.type);
console.log(d instanceof Dog, d instanceof Animal, Object.getPrototypeOf(Dog) === Animal, Dog.name, typeof Dog, String(d), `${a}`);
console.log(Dog.create('Fido').name, Animal.getSp(), a.hasSecret(d), a.hasSecret({}), Object.keys(d), JSON.stringify(d));
try { Dog() } catch (e) { console.log(e.name, e.message) }
try { a.callPriv.call({}) } catch (e) { console.log(e.name, e.message) }
class Base { constructor() { this.init(); } init() { this.v = 'base'; } }
class Derived extends Base { field = 'f'; init() { this.v = 'derived:' + this.field; } }
console.log(new Derived().v);
class NoCtor extends Base {} console.log(new NoCtor().v, NoCtor.length);
class Shape { area() { return 0; } static [Symbol.hasInstance](x) { return 'area' in x; } }
console.log({ area: 1 } instanceof Shape, [] instanceof Shape);
const Mixin = B => class extends B { mixed() { return 'mixed+' + super.speak(); } };
class Mixed extends Mixin(Animal) {} console.log(new Mixed('M').mixed());
class Counter { static #instances = 0; static inc() { return ++Counter.#instances; } } Counter.inc(); console.log(Counter.inc());
class Temp { static { this.initialized = 'yes'; } static val = this.initialized + '!'; } console.log(Temp.initialized, Temp.val);
class Acc { #x = 1; get x() { return this.#x; } static check(o) { try { return o.#x; } catch (e) { return e.constructor.name; } } } console.log(new Acc().x, Acc.check({}));
class ExtArr extends Array { sum() { return this.reduce((s, x) => s + x, 0); } }
const ea = ExtArr.from([1, 2, 3]); console.log(ea.sum(), ea.length, ea instanceof ExtArr, ea.map(x => x * 2) instanceof ExtArr, Array.isArray(ea));
class MyErr extends Error { constructor(m) { super(m); this.name = 'MyErr'; } }
try { throw new MyErr('boom') } catch (e) { console.log(e instanceof MyErr, e instanceof Error, e.name, e.message, String(e)) }
const Anon = class {}; const Named = class Inner { who() { return Inner.name; } }; console.log(Anon.name, Named.name, new Named().who());
class GS { static get sv() { return 'static getter'; } get ['comp' + 'uted']() { return 'cg'; } } console.log(GS.sv, new GS().computed);
console.log(Object.getOwnPropertyNames(Dog.prototype), Object.getOwnPropertyNames(Animal.prototype).sort());
class NT { constructor() { this.nt = new.target.name; } } class NT2 extends NT {} console.log(new NT().nt, new NT2().nt);
class Arrowy { v = 5; f = () => this.v; } const ar = new Arrowy(); const ff = ar.f; console.log(ff());
class SuperProp { m() { return 'parent'; } } class SubProp extends SuperProp { m() { const g = () => super.m(); return g() + '>child'; } } console.log(new SubProp().m());
class Ctr { constructor() { return { replaced: true }; } } console.log(new Ctr().replaced);
class Bad extends Base { constructor() { } } try { new Bad() } catch (e) { console.log(e.name, e.message) }
class Twice extends Base { constructor() { super(); try { super(); } catch (e) { console.log(e.name, e.message) } } } new Twice();
console.log(typeof class {}, class {}.constructor === Function, (class { static x = 1 }).x);
class Point { constructor(x, y) { Object.assign(this, { x, y }); } static from([x, y]) { return new Point(x, y); } plus(o) { return new Point(this.x + o.x, this.y + o.y); } }
console.log(JSON.stringify(Point.from([1, 2]).plus(new Point(3, 4))));
