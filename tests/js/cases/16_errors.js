const errs = [new Error('e'), new TypeError('t'), new RangeError('r'), new SyntaxError('s'), new ReferenceError('re'), new URIError('u'), new EvalError('ev'), new AggregateError([1], 'agg')];
for (const e of errs) console.log(e.name, e.message, String(e), e instanceof Error, Object.prototype.toString.call(e), typeof e.stack, e.constructor.name);
console.log(new Error('with cause', { cause: 'c' }).cause, 'cause' in new Error('x'), new Error().message === '', Error('no new').message, Error.prototype.name, TypeError.prototype instanceof Error);
function f() { try { return 'try'; } finally { console.log('finally runs'); } } console.log(f());
function g() { try { throw 1; } catch (e) { return 'catch ' + e; } finally { console.log('g finally'); } } console.log(g());
function h() { try { return 1; } finally { return 2; } } console.log(h());
function k() { for (let i = 0; i < 3; i++) { try { if (i === 1) continue; if (i === 2) break; console.log('loop', i); } finally { console.log('fin', i); } } return 'k done'; } console.log(k());
function nested() { try { try { throw new Error('inner'); } finally { console.log('inner finally'); } } catch (e) { return 'outer caught ' + e.message; } } console.log(nested());
function rethrow() { try { throw 'a'; } catch (e) { throw e + 'b'; } } try { rethrow(); } catch (e) { console.log(e); }
try { throw { custom: true } } catch ({ custom }) { console.log('destructured catch', custom) }
try { throw null } catch (e) { console.log('null thrown', e) }
try { try { throw 1 } finally { console.log('no catch finally') } } catch (e) { console.log('outer', e) }
const results = []; for (const x of [1, 2, 3]) { try { if (x === 2) throw x; results.push(x); } catch (e) { results.push('err' + e); } } console.log(results.join());
try { undefinedFunction() } catch (e) { console.log(e.name + ': ' + e.message) }
try { let q = 1; q(); } catch (e) { console.log(e.message) }
try { null.foo } catch (e) { console.log(e.message) }
try { undefined.foo = 1 } catch (e) { console.log(e.message) }
try { const c = 1; c = 2; } catch (e) { console.log(e.name, e.message) }
try { new Array(-5) } catch (e) { console.log(e.name, e.message) }
try { 'abc'.repeat(-1) } catch (e) { console.log(e.name, e.message) }
try { Symbol() + '' } catch (e) { console.log(e.name, e.message) }
try { BigInt(1.5) } catch (e) { console.log(e.name, e.message) }
try { ({}).x.y } catch (e) { console.log(e.message) }
try { [].reduce(() => 0) } catch (e) { console.log(e.message) }
try { new (class A { constructor() { throw new Error('ctor') } }) } catch (e) { console.log(e.message) }
try { JSON.parse('{bad') } catch (e) { console.log(e.name) }
try { eval('let let = 1') } catch (e) { console.log(e.name) }
try { eval('1 +') } catch (e) { console.log(e.name, e.message) }
try { eval('function(){}') } catch (e) { console.log(e.name, e.message) }
try { eval('"unterminated') } catch (e) { console.log(e.name, e.message) }
try { eval('x = )') } catch (e) { console.log(e.name, e.message) }
try { eval('let a; let a;') } catch (e) { console.log(e.name, e.message) }
try { eval('break;') } catch (e) { console.log(e.name, e.message) }
try { eval('return 1') } catch (e) { console.log(e.name, e.message) }
try { eval('a => { ') } catch (e) { console.log(e.name, e.message) }
try { eval('({a:1} = 1)') } catch (e) { console.log(e.name, e.message) }
try { eval('1 = 2') } catch (e) { console.log(e.name, e.message) }
class ValidationError extends Error { constructor(field) { super('invalid ' + field); this.name = 'ValidationError'; this.field = field; } }
try { throw new ValidationError('email') } catch (e) { console.log(e.name, e.message, e.field, e instanceof ValidationError, e.toString()) }
const e2 = new Error('props'); e2.code = 42; console.log(Object.keys(e2), e2.code, Object.getOwnPropertyNames(e2).sort());
console.log(Error.prototype.toString.call({ name: 'N', message: 'M' }), Error.prototype.toString.call({}), Error.prototype.toString.call({ message: 'only' }));
function deepThrow(n) { if (n === 0) throw new Error('deep'); return deepThrow(n - 1); } try { deepThrow(100) } catch (e) { console.log(e.message) }
console.log((() => { try { return 'a'; } finally { console.log('arrow finally'); } })());
const gen = (function* () { try { yield 1; } finally { console.log('gen cleanup'); } })(); gen.next(); gen.return();
