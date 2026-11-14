const d = new Date(Date.UTC(2024, 1, 29, 13, 45, 30, 123));
console.log(d.getTime(), d.toISOString(), d.toJSON(), d.toString(), d.toDateString(), d.toTimeString(), d.toUTCString());
console.log(d.getFullYear(), d.getMonth(), d.getDate(), d.getDay(), d.getHours(), d.getMinutes(), d.getSeconds(), d.getMilliseconds(), d.getTimezoneOffset(), d.getUTCDay());
console.log(d.toLocaleDateString(), d.toLocaleTimeString(), d.toLocaleString(), String(d) === d.toString(), d.valueOf() === +d);
const parses = ['2024-01-15', '2024-01-15T10:20:30Z', '2024-01-15T10:20:30.5+02:00', '2024-01-15T10:20', '2024-01', '2024', '+002024-01-01T00:00:00Z',
  'Jan 15, 2024', 'January 15, 2024 10:30:00', '15 Jan 2024', 'Mon, 15 Jan 2024 10:20:30 GMT', 'Mon Jan 15 2024 10:20:30 GMT+0000 (Coordinated Universal Time)',
  '2024/01/15', '01/15/2024', '1/2/2024 3:04 PM', 'garbage', '2024-13-01', '2024-02-30', 'Sat, 01 Jan 2000 00:00:00 GMT+0100', '2024-01-15 10:20:30', 'Dec 25 1995 13:30 UTC'];
for (const s of parses) console.log(JSON.stringify(s), Date.parse(s), new Date(s).getTime());
console.log(new Date(2020, 0, 31).getMonth(), new Date(2020, 0, 31 + 1).toISOString(), new Date(2020, 12, 1).toISOString(), new Date(99, 0).getFullYear(), new Date(0, 0).getFullYear());
const e = new Date(0); e.setFullYear(2000); e.setMonth(5, 15); e.setHours(25, 61, 61, 1001); console.log(e.toISOString());
e.setDate(0); console.log(e.toISOString()); e.setTime(86400000 * 365.25 * 30); console.log(e.toISOString(), e.getDay());
console.log(new Date(NaN).getTime(), String(new Date(NaN)), new Date(NaN).getFullYear(), isNaN(new Date('x')));
try { new Date(NaN).toISOString() } catch (err) { console.log(err.name, err.message) }
console.log(Date.UTC(2024, 0), Date.UTC(1970, 0, 1, 0, 0, 0, 1), new Date(8.64e15).toISOString(), new Date(8.64e15 + 1).getTime(), new Date(-1).toISOString(), new Date(-62198755200000).toISOString());
console.log(typeof Date.now(), Date.now() > 1.6e12, typeof Date(), new Date(new Date(5)).getTime(), new Date(2020, 1, 1, 12).getTime());
const days = []; for (let i = 0; i < 7; i++) days.push(new Date(Date.UTC(2024, 0, 1 + i)).toUTCString().slice(0, 3)); console.log(days.join());
console.log(JSON.stringify({ when: new Date(1e12) }), new Date(1e12) - new Date(5e11), new Date(1e12) > new Date(5e11), new Date(3) + 0 === String(new Date(3)) + '0');
console.log(new Date(Date.UTC(-1, 0)).toISOString(), new Date(Date.UTC(10000, 0)).toISOString(), new Date(Date.UTC(1, 0)).getUTCFullYear());
console.log(Object.prototype.toString.call(new Date()), new Date(2024, 0, 1)[Symbol.toPrimitive]('number'), new Date(0)[Symbol.toPrimitive]('default'));
