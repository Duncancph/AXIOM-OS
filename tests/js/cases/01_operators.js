// arithmetic, coercion, equality, bitwise, comparison
const vals = [0, -0, 1, -1, 1.5, NaN, Infinity, -Infinity, '', '0', '1', 'abc', ' 12 ', true, false, null, undefined, [], [0], [1,2], {}, '0x10', '1e3'];
const names = ['0','-0','1','-1','1.5','NaN','Inf','-Inf',"''","'0'","'1'","'abc'","' 12 '",'true','false','null','undef','[]','[0]','[1,2]','{}',"'0x10'","'1e3'"];
for (let i = 0; i < vals.length; i++) {
  const v = vals[i];
  console.log(names[i], typeof v, String(+v), String(!v), String(v == 0), String(v === 0), String(v == null), String(v == ''), String(-v), String(~v), String(v | 0), String(v >>> 0));
}
for (let i = 0; i < vals.length; i += 3) for (let j = 0; j < vals.length; j += 4) {
  const a = vals[i], b = vals[j];
  console.log(names[i], names[j], String(a == b), String(a === b), String(a < b), String(a >= b), String(a + b), String(a - b), String(a * b));
}
console.log(1 + 2 + '3', '1' + 2 + 3, 1 + +'2', '3' - - '3', 10 / 3, 10 % 3, -10 % 3, 10 % -3, 5.5 % 2, 2 ** 3 ** 2, (-2) ** 2);
console.log(1 << 31, 1 << 32, -1 >>> 0, -1 >> 31, 0xFFFFFFFF | 0, 2147483648 | 0, 4294967296.5 | 0, -2147483649 | 0);
console.log(7 & 3, 7 | 8, 7 ^ 2, ~7, ~~3.7, ~~-3.7, 1e21 | 0, 123456789012 | 0);
console.log(null + 1, undefined + 1, true + true, [] + [], [1] + [2], {} + 1, 'b' + 'a' + +'a' + 'a');
console.log('10' < '9', 10 < 9, '10' < 9, 'a' < 'b', 'B' < 'a', null >= 0, null > 0, null == 0, undefined == 0, NaN != NaN);
console.log([2] * [3], [1,2] * 2, '4' / '2', '0.1' * 3, 0.1 * 3, 1 / 0, -1 / 0, 0 / 0, 1 / -0);
let a = 5; a += 2; a -= 1; a *= 3; a /= 2; a %= 4; a **= 2; console.log(a);
let b = 0b1010; b <<= 2; b >>= 1; b >>>= 1; b &= 6; b |= 1; b ^= 3; console.log(b);
let c = null; c ??= 5; c ||= 6; c &&= 7; console.log(c);
let d = 0; d ||= 3; let e = 1; e &&= 0; let f = 0; f ??= 9; console.log(d, e, f);
console.log(void 0, typeof void 0, (1, 2, 3), !!'false', !!0n, !!'', !!NaN);
var i = 1; console.log(i++ + ++i, i, i-- - --i, i);
var o = { valueOf() { return 42; }, toString() { return 'str'; } };
console.log(o + 1, `${o}`, o * 2, String(o), o > 41, o == 42, o + '');
var o2 = { [Symbol.toPrimitive](hint) { return hint === 'number' ? 1 : hint === 'string' ? 'S' : 'D'; } };
console.log(+o2, `${o2}`, o2 + '', o2 * 3);
console.log('5' == 5, '' == 0, '0' == false, [] == false, [0] == false, [[]] == 0, null == false, undefined == null, NaN == NaN);
console.log(typeof (() => {}), typeof class {}, typeof [], typeof new Number(1), typeof Object(1n), typeof NaN);
console.log(1 < 2 < 3, 3 > 2 > 1, '2' > '12', 2 > '12', 'abc'.length * 2);
console.log(0.1 + 0.2 === 0.3, Math.abs(0.1 + 0.2 - 0.3) < Number.EPSILON, 9007199254740993, 2 ** 53 + 1);
console.log(-(2 ** 31), 2 ** -1074, 2 ** 1023 * 2, -(2 ** 1024));
