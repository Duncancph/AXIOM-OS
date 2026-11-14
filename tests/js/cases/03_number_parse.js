const strs = ['', ' ', '0', '-0', '+5', '  42  ', '1e3', '1E-3', '.5', '5.', '0x1F', '0X1f', '0o17', '0b101', '-0x10', '0x', '1_000',
  'Infinity', '-Infinity', 'infinity', 'NaN', '12px', 'px12', '3.14abc', '\n\t 7 \r', '1e', '1e+', '00012', '0.0000001', '9007199254740993',
  '123456789012345678901234567890', '1.7976931348623157e308', '1.8e308', '4.9e-324', '2.5e-324', '1e-400', '0.1', '.1e1', '1.', '+.5', '-.5e-2'];
for (const s of strs) console.log(JSON.stringify(s), Number(s), parseFloat(s), parseInt(s), parseInt(s, 16), +s);
console.log(parseInt('101', 2), parseInt('ff', 16), parseInt('0xff', 16), parseInt('z', 36), parseInt('12', 3), parseInt('1', 1), parseInt('10', 37), parseInt('  -12abc'), parseInt('1e3'), parseInt(0.0000005), parseInt('0.9'));
console.log(parseFloat('1e1000'), parseFloat('-0'), parseFloat('  .5e1x'), parseFloat('Infinityx'), parseFloat('-Infinity'), parseFloat('0x10'));
console.log(0xff, 0o777, 0b1111, 1_000_000, 1e1_0, 0.000_001, 017, 019, 08.5, 0.1e-1);
console.log(Number.isInteger(5), Number.isInteger(5.5), Number.isInteger('5'), Number.isSafeInteger(2 ** 53), Number.isSafeInteger(2 ** 53 - 1), Number.isFinite('5'), isFinite('5'), Number.isNaN('x'), isNaN('x'));
console.log(Number(null), Number(undefined), Number(true), Number([]), Number([5]), Number([1, 2]), Number({}), Number(new Date(0)), Number('    3 ﻿'));
console.log(0.1 + 0.7, 1.0000000000000002, 1.0000000000000001, 0.30000000000000004, 1e23, 8.98846567431158e307, 2.2250738585072014e-308, 2.225073858507201e-308);
for (const s of ['123.456e5', '9.999999999999999e22', '1.23456789012345678', '0.1234567890123456789', '5e-324', '1e-323', '2.4703282292062327e-324', '2.4703282292062328e-324', '7.2057594037927933e16', '18446744073709551616', '9223372036854775807', '0.000000000000000000000000000000001'])
  console.log(s, Number(s));
