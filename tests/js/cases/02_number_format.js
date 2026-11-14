const nums = [0, -0, 1, -1, 0.1, 0.2, 0.3, 0.1 + 0.2, 1 / 3, 2 / 3, 10 / 3, 123.456, 1e21, 1e-7, 1.5e-7, 123e20, 1e300, 5e-324,
  1.7976931348623157e308, 2 ** 53, 2 ** 53 + 2, 123456789.123456789, 0.000001, 0.0000001, 1e100, 4.35, 0.5, 1.45, 2.5, -2.5,
  1.005, 100, 1000000000000000000000, 999999999999999999999, 1.23e-10, 3.14159, Math.PI, Math.E, 12345.6789, 0.1 * 3, 1.1 * 1.1, 4.4 - 3.3];
for (const n of nums) console.log(String(n), n.toFixed(2), n.toFixed(0), n.toExponential(), n.toExponential(3), n.toPrecision(4), n.toPrecision(1), n.toString(16), n.toString(2).slice(0, 40), n.toString(36));
for (const n of [0.5, 1.5, 2.5, 3.5, -0.5, -1.5, 1.005, 1.255, 8.345, 0.045, 10.235, 1e-10, 123.456]) console.log(n.toFixed(1), n.toFixed(2), n.toFixed(5), n.toFixed(20));
for (const n of [1, 12, 123, 1234, 12345, 123456, 1e6, 1.5e6, 0.001234, 99.99]) console.log(n.toPrecision(2), n.toPrecision(3), n.toPrecision(7), n.toExponential(0), n.toExponential(1));
console.log((255).toString(16), (255).toString(2), (-255).toString(36), (0.5).toString(2), (0.1).toString(3), (3.75).toString(8), (1e21).toString(36), (123.456).toString(7));
console.log(Number.MAX_VALUE, Number.MIN_VALUE, Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER, Number.EPSILON);
console.log((25).toString(), (1e-6).toString(), (1e-7).toString(), (123e-20).toString(), (1.5e300 * 1.5e10).toString());
console.log([1.1, 2.2, 3.3].map(x => x * 3).join(' '), 0.7 + 0.1, 0.2 + 0.4, 1.1 + 2.2, 3 * 1.1, 100 * 1.1, 0.57 * 100);
for (let i = 1; i < 30; i++) console.log(i, 1 / i, Math.sqrt(i), i / 7, i * 0.1, 2 ** (i / 3));
try { (1).toFixed(101) } catch (e) { console.log(e.name, e.message) }
try { (1).toString(1) } catch (e) { console.log(e.name, e.message) }
try { (1).toPrecision(0) } catch (e) { console.log(e.name, e.message) }
console.log((1234.5678).toFixed(2), (-1234.5678).toFixed(1), (0).toFixed(3), (-0).toFixed(2), (NaN).toFixed(2), (1e21).toFixed(2), (Infinity).toExponential());
console.log((1e21).toPrecision(3), (123456).toPrecision(2), (0.00001).toPrecision(1), (0.000001).toPrecision(2), (1e-7).toPrecision(3));
