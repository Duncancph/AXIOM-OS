const r = x => Math.round(x * 1e10) / 1e10;
for (const x of [0, 0.5, 1, 2, 3.7, -1.5, 10, 100, 0.001, 1e-10, Math.PI / 4, -0]) {
  console.log(x, Math.sqrt(Math.abs(x)), r(Math.sin(x)), r(Math.cos(x)), r(Math.tan(x)), r(Math.exp(x)), r(Math.log(Math.abs(x) + 1)), r(Math.atan(x)), r(Math.cbrt(x)), r(Math.sinh(x)), r(Math.tanh(x)));
}
console.log(Math.floor(-1.5), Math.ceil(-1.5), Math.round(-1.5), Math.round(2.5), Math.round(-2.5), Math.round(0.49999999999999994), Math.trunc(-4.7), Math.sign(-3), Math.sign(0), Math.sign(-0));
console.log(Math.max(), Math.min(), Math.max(1, NaN), Math.max(-0, 0), Math.min(0, -0), Math.max(...[3, 7, 2]), Math.abs(-7.5), Math.hypot(3, 4), Math.hypot(), Math.hypot(1, 2, 2));
console.log(Math.pow(2, 10), Math.pow(2, -2), Math.pow(-8, 1 / 3), Math.pow(NaN, 0), Math.pow(1, Infinity), 2 ** 0.5, 10 ** -5, 1.1 ** 10, Math.pow(10, 21), Math.pow(10, 22), Math.pow(10, 23));
console.log(Math.log2(8), Math.log2(1024), Math.log10(1000), Math.log10(0.001), Math.log1p(0), Math.expm1(0), Math.log(Math.E), Math.exp(1), Math.log(10), Math.log2(3));
console.log(Math.clz32(1), Math.clz32(0), Math.clz32(-1), Math.imul(3, 4), Math.imul(0xffffffff, 5), Math.fround(5.5), Math.fround(5.05), Math.fround(2 ** 128));
console.log(Math.asin(1), Math.acos(-1), Math.atan2(1, 1), Math.atan2(-0, -1), Math.atan2(0, -0), Math.asinh(1), Math.acosh(2), Math.atanh(0.5), Math.cosh(1));
console.log(Math.PI, Math.E, Math.LN2, Math.LN10, Math.LOG2E, Math.LOG10E, Math.SQRT2, Math.SQRT1_2, String(Math));
console.log(Math.sin(Math.PI), Math.cos(Math.PI), Math.sin(1e6), Math.cos(1e10), Math.sin(1e300), Math.tan(Math.PI / 2), Math.exp(710), Math.exp(-746), Math.log(-1), Math.sqrt(-1));
for (let i = -5; i <= 5; i++) console.log(i, Math.exp(i), Math.pow(1.5, i), Math.sin(i), Math.cos(i), Math.atan(i), Math.cbrt(i * 7));
const rnd = []; for (let i = 0; i < 1000; i++) rnd.push(Math.random()); console.log(rnd.every(x => x >= 0 && x < 1), new Set(rnd).size > 990);
console.log(0.1 * 0.2, 1 / 3 * 3, 4.35 * 100, 1.15 * 100, Math.round(1.005 * 100) / 100, (0.1).toFixed(20), 3 * 0.1, 0.3 - 0.1);
console.log(Math.sqrt(2), Math.sqrt(1e-300), Math.sqrt(Number.MAX_VALUE), Math.cbrt(-27), Math.cbrt(2), Math.exp(0.5), Math.log(0.5), Math.sinh(-2), Math.tanh(30));
