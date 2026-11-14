const a = 12345678901234567890n, b = 987654321n;
console.log(a + b, a - b, a * b, a / b, a % b, -a, a ** 3n, 2n ** 100n, (2n ** 64n) - 1n, typeof a, String(a), a.toString(16), (-255n).toString(2));
console.log(10n / 3n, -10n / 3n, 10n % -3n, -10n % 3n, 5n << 10n, -5n >> 1n, 1024n >> 3n, 5n & 3n, 5n | 8n, 5n ^ 1n, ~5n, -(-5n), ~-1n, -7n & 0xFFn, -1n | 4n, (-6n) ^ 3n);
console.log(1n < 2, 2n > 1.5, 1n == 1, 1n === 1, 0n == '', 10n == '10', 3n > '2', BigInt(9007199254740993), BigInt('0x1f'), BigInt('  42  '), BigInt(true), BigInt(-0));
console.log(BigInt.asIntN(8, 255n), BigInt.asUintN(8, -1n), BigInt.asIntN(64, 2n ** 63n), BigInt.asUintN(64, -1n), Number(2n ** 53n + 1n), Number(-123n), parseInt('123n'), Number(10n ** 30n));
console.log([3n, 1n, 2n].sort().join(), [10n, 9n, 100n].sort((x, y) => (x < y ? -1 : x > y ? 1 : 0)).join(), 0n ? 'truthy' : 'falsy', 1n && 'yes', !0n);
try { 1n + 1 } catch (e) { console.log(e.name, e.message) }
try { BigInt('1.5') } catch (e) { console.log(e.name, e.message) }
try { 1n / 0n } catch (e) { console.log(e.name, e.message) }
try { Math.max(1n) } catch (e) { console.log(e.name, e.message) }
try { +1n } catch (e) { console.log(e.name, e.message) }
try { JSON.stringify(1n) } catch (e) { console.log(e.name, e.message) }
let fact = 1n; for (let i = 1n; i <= 30n; i++) fact *= i; console.log(fact, fact.toString().length);
let x = 5n; x++; x += 2n; x **= 2n; console.log(x, typeof Object(1n), Object(2n) + 1n, `${123n}`, 1n.toLocaleString === undefined, (12345678n).toLocaleString());
console.log(0x10n, 0o17n, 0b101n, 1_000n, -(2n ** 200n), (2n ** 200n) / (3n ** 50n), (10n ** 40n + 7n) % 97n);
