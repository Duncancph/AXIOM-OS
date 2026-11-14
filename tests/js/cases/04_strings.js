const s = 'Hello, World';
console.log(s.length, s.charAt(4), s.charCodeAt(7), s.codePointAt(0), s.at(-1), s.at(100), s[1], s['length']);
console.log(s.indexOf('o'), s.indexOf('o', 5), s.lastIndexOf('o'), s.lastIndexOf('o', 5), s.indexOf(''), s.indexOf('zz'), s.includes('World'), s.startsWith('Hell'), s.endsWith('ld'), s.startsWith('World', 7), s.endsWith('Hello', 5));
console.log(s.slice(7), s.slice(-5), s.slice(2, -2), s.substring(7, 2), s.substring(-3), s.substr(-5, 3), s.substr(2), s.slice(5, 2));
console.log(s.toUpperCase(), s.toLowerCase(), '  pad  '.trim() + '|', '  pad  '.trimStart() + '|', '  pad  '.trimEnd() + '|');
console.log('5'.padStart(3, '0'), 'abc'.padEnd(8, '12'), 'x'.padStart(5), 'long'.padStart(2), 'ab'.repeat(3), ''.repeat(5), 'a'.repeat(0));
console.log('a,b,,c'.split(','), 'abc'.split(''), 'a b  c'.split(' '), 'abc'.split(), 'a,b,c'.split(',', 2), ''.split(','), ''.split(''), 'abc'.split('b'));
console.log('aaa'.replace('a', 'b'), 'aaa'.replaceAll('a', 'b'), 'a-b-c'.replace('-', '$$'), 'abc'.replace('b', '[$&]'), 'abc'.replace('b', "$`|$'"), 'x'.replace('x', () => '$&'));
console.log('abc'.concat('def', 1, null), 'a'.localeCompare('b'), 'b'.localeCompare('a'), 'a'.localeCompare('a'), 'a'.localeCompare('B'), 'résumé'.normalize());
console.log(String.fromCharCode(72, 105, 0x1F600 & 0xffff), String.fromCodePoint(0x1F600, 65), String.raw`a\nb${1}c`, String(123), String(null), String([1, [2, 3]]), String({}));
console.log('ß'.toUpperCase(), 'İ'.toLowerCase().length, 'ΣΑΣ'.toLowerCase(), 'Ǆ'.toLowerCase(), 'straße'.toUpperCase(), 'ÀÉÎõü'.toLowerCase(), 'àéîÕÜ'.toUpperCase(), 'ÿ'.toUpperCase());
const emoji = 'a😀b';
console.log(emoji.length, [...emoji].length, emoji.charCodeAt(1), emoji.codePointAt(1), emoji.codePointAt(2), emoji.split(''), Array.from(emoji), emoji.at(1) === '\ud83d');
console.log('abc' < 'abd', 'abc' < 'ab', 'Z' < 'a', 'é' > 'z', ['b', 'a', 'C', 'é'].sort(), ['b', 'a', 'C', 'é'].sort((x, y) => x.localeCompare(y)));
console.log('%', 'a\tb', 'quote"s', "it's", `back\`tick`, '\x41B\u{43}', '\0'.length, 'a\
b');
console.log('Hello'.search('l'), 'x'.isWellFormed(), '\ud800'.isWellFormed(), '\ud800x'.toWellFormed().charCodeAt(0));
let big = ''; for (let i = 0; i < 1000; i++) big += i; console.log(big.length, big.slice(0, 20), big.slice(-10));
const words = 'the quick brown fox jumps over the lazy dog'.split(' ');
console.log(words.map(w => w[0].toUpperCase() + w.slice(1)).join(' '), words.filter(w => w.length > 3).length);
console.log('abc'.anchor('x'), 'b'.bold(), 'A-B_C d'.split(/[-_ ]/), 'camelCaseString'.replace(/([A-Z])/g, ' $1').toLowerCase());
console.log([...'abc'].reverse().join(''), 'abc'.split('').map(c => c.charCodeAt(0)), escape('a b+c/ä€'), unescape('%u20AC%41%zz'));
console.log(encodeURIComponent('a b&c=d/é€😀'), encodeURI('http://x.com/a b?q=1&r=é#h'), decodeURIComponent('%E2%82%AC%20x'), decodeURI('%3Fa%20b'));
try { decodeURIComponent('%E2%82') } catch (e) { console.log(e.name, e.message) }
console.log('abc'.substring(NaN, 2), 'abc'.slice(NaN), 'abc'.charAt(-1) === '', 'abc'.indexOf('c', -5), 'aXbXc'.split('X', -1));
