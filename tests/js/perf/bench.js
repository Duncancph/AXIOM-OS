// Performance check: prints RESULT lines (compared with node) and TIME lines (tabulated)
function time(name, f) { const t0 = Date.now(); const r = f(); console.log('RESULT ' + name + ' ' + r); console.log('TIME ' + name + ' ' + (Date.now() - t0)); }
time('fib25', () => { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(25); });
time('loop1e6', () => { let s = 0; for (let i = 0; i < 1e6; i++) s += i % 7; return s; });
time('sort100k', () => { const a = []; let x = 12345; for (let i = 0; i < 100000; i++) { x = (x * 1103515245 + 12345) % 2147483648; a.push(x); } a.sort((p, q) => p - q); return a[0] + ',' + a[50000] + ',' + a[99999]; });
time('strbuild', () => { let s = ''; for (let i = 0; i < 100000; i++) s += String.fromCharCode(97 + i % 26); return s.length + ':' + s.slice(-5); });
time('arrayjoin', () => { const parts = []; for (let i = 0; i < 100000; i++) parts.push('item' + i); return parts.join(',').length; });
time('objprops', () => { let sum = 0; for (let i = 0; i < 200000; i++) { const o = { x: i, y: i * 2, z: 'p' }; sum += o.x + o.y; } return sum; });
time('classes', () => { class V { constructor(x, y) { this.x = x; this.y = y; } add(o) { return new V(this.x + o.x, this.y + o.y); } len() { return Math.sqrt(this.x * this.x + this.y * this.y); } } let v = new V(0, 0); for (let i = 0; i < 200000; i++) v = v.add(new V(1, 1)); return v.len().toFixed(3); });
time('closures', () => { const fs = []; for (let i = 0; i < 100000; i++) fs.push(() => i * 2); let s = 0; for (const f of fs) s += f(); return s; });
time('mapset', () => { const m = new Map(); for (let i = 0; i < 100000; i++) m.set('k' + (i % 5000), i); const s = new Set(); for (let i = 0; i < 100000; i++) s.add(i % 777); return m.size + ':' + s.size + ':' + m.get('k42'); });
time('json', () => { const o = { list: Array.from({ length: 2000 }, (_, i) => ({ id: i, name: 'n' + i, tags: ['a', 'b'], v: i / 3 })) }; let n = 0; for (let i = 0; i < 10; i++) n += JSON.parse(JSON.stringify(o)).list.length; return n; });
time('regex', () => { const text = 'The year 1999 and 2024, emails a@b.com x@y.org; '.repeat(2000); let n = 0; n += (text.match(/\d{4}/g) || []).length; n += text.replace(/[aeiou]/g, '').length; n += text.split(/[,;]\s*/).length; return n; });
time('arraymethods', () => { const a = Array.from({ length: 100000 }, (_, i) => i); return a.map(x => x * 2).filter(x => x % 3 === 0).reduce((s, x) => s + x, 0); });
time('generators', () => { function* g(n) { for (let i = 0; i < n; i++) yield i; } let s = 0; for (const v of g(200000)) s += v; return s; });
time('globalcalls', () => { let c = 0; for (let i = 0; i < 300000; i++) c += Math.max(i & 7, 3); return c; });
