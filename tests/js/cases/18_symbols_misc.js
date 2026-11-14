const s1 = Symbol('desc'), s2 = Symbol('desc'), s3 = Symbol();
console.log(s1 === s2, s1.toString(), s1.description, s3.description, String(s3), typeof s1, Symbol.for('k') === Symbol.for('k'), Symbol.keyFor(Symbol.for('k')), Symbol.keyFor(s1));
const o = { [s1]: 'v1', regular: 1 }; console.log(o[s1], Object.keys(o), Object.getOwnPropertySymbols(o).length, JSON.stringify(o), s1 in o, Reflect.ownKeys(o).length);
console.log(Symbol.iterator.toString(), typeof Symbol.asyncIterator, Symbol.hasInstance.description, Object(s1) instanceof Symbol, Object(s1) == s1);
try { new Symbol() } catch (e) { console.log(e.name, e.message) }
try { `${s1}` } catch (e) { console.log(e.name, e.message) }
try { s1 + 1 } catch (e) { console.log(e.name, e.message) }
class Coll { constructor() { this.items = [1, 2, 3]; } *[Symbol.iterator]() { yield* this.items; } get [Symbol.toStringTag]() { return 'Coll'; } }
console.log([...new Coll()], String(new Coll()), Object.prototype.toString.call(new Coll()));
const spreadable = { length: 2, 0: 'a', 1: 'b', [Symbol.isConcatSpreadable]: true }; console.log([1].concat(spreadable));
const arrNoSpread = [3, 4]; arrNoSpread[Symbol.isConcatSpreadable] = false; console.log([1].concat(arrNoSpread).length);
const prim = { [Symbol.toPrimitive](h) { return h === 'number' ? 7 : 'p:' + h; } }; console.log(+prim, `${prim}`, prim + '', prim * 1, [prim] + '');
console.log(Symbol('x').description, Symbol().description, Symbol.prototype[Symbol.toStringTag], Object.getOwnPropertyNames(Symbol).includes('iterator'));
const wk = [Symbol.iterator, Symbol.asyncIterator, Symbol.hasInstance, Symbol.toPrimitive, Symbol.toStringTag, Symbol.species, Symbol.isConcatSpreadable, Symbol.unscopables, Symbol.match, Symbol.matchAll, Symbol.replace, Symbol.search, Symbol.split];
console.log(wk.map(s => s.description).join());
class Even { static [Symbol.hasInstance](n) { return n % 2 === 0; } } console.log(2 instanceof Even, 3 instanceof Even);
const customReplace = { [Symbol.replace](s, r) { return s.toUpperCase() + r; } }; console.log('abc'.replace(customReplace, '!'));
const customSplit = { [Symbol.split](s) { return s.split('').reverse(); } }; console.log('abc'.split(customSplit));
console.log(globalThis === this, typeof globalThis, Object.keys(globalThis).length >= 0);
console.log([1, 2, 3].includes(2), 'includes' in Array.prototype, Array.prototype[Symbol.unscopables].includes, Symbol.unscopables in Array.prototype);
