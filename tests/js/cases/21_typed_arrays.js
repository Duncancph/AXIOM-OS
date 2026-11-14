const u8 = new Uint8Array([1, 2, 255, 256, -1]); console.log(u8.length, Array.from(u8), u8[3], u8.byteLength, Object.prototype.toString.call(u8));
const i8 = new Int8Array([127, 128, -129]); const u8c = new Uint8ClampedArray([300, -5, 1.5, 2.5, 0.5]); console.log(Array.from(i8), Array.from(u8c));
const f32 = new Float32Array([1.1, 0.5, 1e40]); const f64 = new Float64Array([1.1, NaN]); console.log(Array.from(f32), Array.from(f64));
const buf = new ArrayBuffer(8); const dv = new DataView(buf); dv.setUint16(0, 0x1234); dv.setUint16(2, 0x1234, true); dv.setFloat32(4, 2.5);
console.log(new Uint8Array(buf).join(','), dv.getUint16(0), dv.getUint16(0, true), dv.getInt8(1), dv.getFloat32(4), dv.byteLength, buf.byteLength);
const i32 = new Int32Array(buf); console.log(i32.length, i32[0], new Uint32Array([-1])[0], new Int16Array([40000])[0]);
const sub = u8.subarray(1, 3); sub[0] = 99; console.log(u8[1], sub.length, u8.slice(1, 3).join(), u8.indexOf(255), u8.includes(0));
const ta = new Int16Array(5).fill(7); ta.set([1, 2], 3); console.log(ta.join(), ta.map(x => x * 2).join(), ta.filter(x => x > 5).length, ta.reduce((a, b) => a + b));
const sorted = new Float64Array([3, -1, NaN, 0, -0, 2]).sort(); console.log(Array.from(sorted), Object.is(sorted[1], -0));
console.log(Uint8Array.from([1, 2, 3], x => x * 3).join(), Int8Array.of(1, -2).join(), Uint8Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT, new Uint16Array(3).byteLength);
const big = new BigInt64Array([1n, -2n, 2n ** 63n - 1n]); console.log(big[1], big[2], typeof big[0], new BigUint64Array([-1n])[0]);
dv.setBigInt64(0, -5n); console.log(dv.getBigInt64(0), dv.getBigUint64(0) > 0n);
try { new Uint8Array(-1) } catch (e) { console.log(e.name, e.message) }
try { dv.getUint32(6) } catch (e) { console.log(e.name, e.message) }
const shared = new Uint8Array(new ArrayBuffer(4), 1, 2); console.log(shared.length, shared.byteOffset, shared.buffer.byteLength, ArrayBuffer.isView(shared), ArrayBuffer.isView([]));
const rev = new Uint8Array([1, 2, 3]).reverse(); console.log(rev.join(), [...rev.entries()].length, [...rev.keys()], rev.find(x => x < 3), rev.findIndex(x => x === 9), rev.every(x => x > 0), rev.at(-1));
console.log(new Uint8Array([5, 10]).toString(), new Float32Array(2).fill(0.1)[0], new Uint8Array(buf.slice(0, 2)).length, new Uint8Array([1, 2, 3, 4]).copyWithin(0, 2).join());
const copyFrom = new Uint8Array(new Uint16Array([300, 2])); console.log(copyFrom.join());
for (const v of new Uint8Array([7, 8])) console.log('iter', v);
console.log(JSON.stringify(new Uint8Array([1, 2])), Object.keys(new Uint8Array(2)), u8 instanceof Uint8Array, Object.getPrototypeOf(Uint8Array).name);
