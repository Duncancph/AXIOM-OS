const tests = [
  [/a+/, 'caaat'], [/(\d+)-(\d+)/, 'tel 555-1234 x'], [/^\w+@\w+\.com$/i, 'Bob@Example.COM'], [/colou?r/g, 'color colour colr'],
  [/(?<year>\d{4})-(?<mon>\d\d)/, 'on 2024-03-15'], [/\bcat\b/, 'concat cat'], [/a.c/s, 'a\nc'], [/a.c/, 'a\nc'], [/^b/m, 'a\nb'],
  [/(a)|(b)/, 'b'], [/(?:ab)+/, 'ababab'], [/a{2,3}/, 'aaaa'], [/a{2,}?/, 'aaaa'], [/a*?b/, 'aaab'], [/(a+)+b/, 'aaab'],
  [/(?=\d)\w+/, 'ab12'], [/(?!a)\w/, 'ab'], [/(?<=\$)\d+/, 'cost $42'], [/(?<!\$)\b\d+/, '$5 and 7'], [/(\w)\1/, 'hello'],
  [/[^aeiou\s]+/g, 'hello world'], [/[a-z\d_-]+/i, '--Ab_9!'], [/\s+/g, 'a \t\n b'], [/[一-鿿]+/, 'abc中文def'],
  [/\u{1F600}/u, 'x😀'], [/./u, '😀'], [/./, '😀'], [/^.$/u, '😀'], [/[😀]/u, '😀'], [/\p{L}+/u, '123héllo'], [/\p{Lu}/u, 'abcD'],
  [/(?<a>x)(?<b>y)?/, 'x'], [/\d+(?:\.\d+)?/g, '1.5 22 3.75'], [/[.*+?^${}()|[\]\\]/g, 'a.b*c'], [/x*/g, 'axx'], [/^$/, ''],
  [/\//, 'a/b'], [/[\b]/, 'a\bb'], [/\cJ/, '\n'], [/\x41B/, 'AB'], [/[\d-x]/, '-'], [/a|ab|abc/, 'abc'], [/(a|ab)(c|bcd)(d*)/, 'abcd'],
  [/((a)|b)+/, 'ab'], [/(z)((a+)?(b+)?(c))*/, 'zaacbbbcac'], [/(.*?)a(?!(a+)b\2c)\2(.*)/, 'baaabaac'], [/[^]/, '\n'], [/\Bo\B/, 'foo'],
];
for (const [re, s] of tests) {
  const m = re.exec(s);
  console.log(String(re), JSON.stringify(s), m ? JSON.stringify([...m]) + ' @' + m.index + (m.groups ? ' ' + JSON.stringify(m.groups) : '') : 'null', re.test(s), s.search(re));
}
console.log('2024-03-15'.replace(/(\d+)-(\d+)-(\d+)/, '$3/$2/$1'), 'aaa'.replace(/a/g, (m, i) => i), 'John Smith'.replace(/(?<f>\w+) (?<l>\w+)/, '$<l>, $<f>'));
console.log('a1b2c3'.replace(/\d/g, d => d * 2), 'x'.replace(/x/, '$0$1$&$$'), 'abc'.replace(/(b)/, "[$`|$'|$1]"), 'aBc'.replace(/b/i, m => m.toUpperCase() + '!'));
console.log('a,b;c d'.split(/[,; ]/), 'a1b22c'.split(/(\d+)/), 'abc'.split(/(?:)/), ''.split(/x/), 'test'.split(/(t)/), 'a, b ,c'.split(/\s*,\s*/, 2));
console.log('one two three'.match(/\w+/g), 'no'.match(/x/g), 'ab12cd'.match(/\d/), JSON.stringify('k=v;x=y'.match(/(\w)=(\w)/)));
const all = [...'a1b2c3'.matchAll(/([a-z])(\d)/g)]; console.log(all.map(m => m[1] + m[2] + m.index).join(' '));
const re = /o/g; console.log(re.test('foo'), re.lastIndex, re.test('foo'), re.lastIndex, re.test('foo'), re.lastIndex);
const sticky = /a/y; sticky.lastIndex = 1; console.log(sticky.test('ba'), sticky.lastIndex, sticky.test('ba'), sticky.lastIndex);
const r2 = new RegExp('a+b', 'gi'); console.log(r2.source, r2.flags, r2.global, r2.ignoreCase, r2.multiline, String(r2), new RegExp(r2).flags, new RegExp(/x/g, 'm').flags, RegExp('/').source, new RegExp('').source);
console.log(/[/]/.source, String(new RegExp('a/b')), /\n/.source, new RegExp('\n').source);
const ri = /(?<w>\w)/d.exec('  x'); console.log(JSON.stringify(ri.indices), JSON.stringify(ri.indices.groups));
try { new RegExp('(') } catch (e) { console.log(e.name, e.message) }
try { new RegExp('a', 'zz') } catch (e) { console.log(e.name, e.message) }
try { 'x'.replaceAll(/x/, 'y') } catch (e) { console.log(e.name, e.message) }
console.log('Hello World'.replace(/o/g, '0'), 'aaa'.replaceAll(/a/g, 'b'), 'x-y_z'.replace(/[-_](.)/g, (_, c) => c.toUpperCase()));
const emailRe = /^[^\s@]+@[^\s@]+\.[^\s@]+$/; console.log(['a@b.co', 'bad@', 'x y@z.com', 'ok.name+tag@sub.domain.org'].map(e => emailRe.test(e)));
console.log('The Quick Brown'.match(/[A-Z]/g).join(''), 'aaa'.lastIndexOf('a'), /(\d{3})-?(\d{4})/.exec('5551234').slice(1));
const longStr = 'ab'.repeat(5000) + 'c'; console.log(/(ab)*c/.test(longStr), longStr.replace(/b/g, '').length, /a.*c$/.test(longStr));
console.log('ABC'.replace(/b/gi, '_'), 'αβγ'.toUpperCase(), /σ/i.test('Σ'), /k/iu.test('K'), /k/i.test('K'));
console.log('2+3=5'.replace(/(\d)\+(\d)=(\d)/, (m, a, b, c, off, str) => [m, a, b, c, off, str].join('|')));
console.log('path/to/file.txt'.match(/([^/]+)\.(\w+)$/).slice(1), '  trim  '.replace(/^\s+|\s+$/g, ''), 'a.b.c'.split('.', 2));
const reLast = /a/g; reLast.lastIndex = 5; console.log('aaa'.replace(reLast, 'b'), reLast.lastIndex, 'xax'.search(/a/g));
console.log(RegExp.prototype.toString.call(/a/g), /a/[Symbol.replace]('aXa', 'b'), RegExp.prototype[Symbol.split].call(/,/, 'a,b'));
console.log(/(?:)/.test(''), /[]/.test('a'), /[^]/.test('a'), /a{,5}/.test('a{,5}'), /{/.test('{'), /x{2}/.exec('xxx')[0]);
