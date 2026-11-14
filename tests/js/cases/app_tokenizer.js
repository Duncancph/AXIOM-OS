// A tokenizer + recursive-descent parser + evaluator for arithmetic with variables and functions
const TOKEN_RE = /\s*(?:(\d+(?:\.\d+)?(?:e[+-]?\d+)?)|([A-Za-z_]\w*)|(\*\*|[-+*/%^(),=]))/gy;
function tokenize(src) {
  const out = []; TOKEN_RE.lastIndex = 0; let m;
  while (TOKEN_RE.lastIndex < src.length && (m = TOKEN_RE.exec(src))) {
    if (m[1]) out.push({ t: 'num', v: parseFloat(m[1]) });
    else if (m[2]) out.push({ t: 'id', v: m[2] });
    else if (m[3]) out.push({ t: 'op', v: m[3] });
  }
  if (TOKEN_RE.lastIndex < src.trimEnd().length) throw new SyntaxError('bad char at ' + TOKEN_RE.lastIndex);
  return out;
}
function parse(tokens) {
  let i = 0;
  const peek = () => tokens[i], next = () => tokens[i++];
  const expect = v => { const t = next(); if (!t || t.v !== v) throw new SyntaxError(`expected ${v} got ${t ? t.v : 'EOF'}`); };
  function primary() {
    const t = next();
    if (!t) throw new SyntaxError('unexpected EOF');
    if (t.t === 'num') return { k: 'num', v: t.v };
    if (t.t === 'id') {
      if (peek()?.v === '(') { next(); const args = []; if (peek()?.v !== ')') { do { args.push(expr()); } while (peek()?.v === ',' && next()); } expect(')'); return { k: 'call', f: t.v, args }; }
      return { k: 'var', n: t.v };
    }
    if (t.v === '(') { const e = expr(); expect(')'); return e; }
    if (t.v === '-') return { k: 'neg', e: unary() };
    throw new SyntaxError('unexpected ' + t.v);
  }
  function unary() { return primary(); }
  function power() { const b = unary(); if (peek()?.v === '^' || peek()?.v === '**') { next(); return { k: 'bin', op: '^', l: b, r: power() }; } return b; }
  function term() { let l = power(); while (['*', '/', '%'].includes(peek()?.v)) { const op = next().v; l = { k: 'bin', op, l, r: power() }; } return l; }
  function expr() { let l = term(); while (['+', '-'].includes(peek()?.v)) { const op = next().v; l = { k: 'bin', op, l, r: term() }; } return l; }
  function stmt() { if (tokens[i]?.t === 'id' && tokens[i + 1]?.v === '=') { const n = next().v; next(); return { k: 'assign', n, e: expr() }; } return expr(); }
  const ast = stmt(); if (i < tokens.length) throw new SyntaxError('trailing ' + tokens[i].v); return ast;
}
const env = { pi: Math.PI, e: Math.E };
const fns = { sqrt: Math.sqrt, max: Math.max, min: Math.min, abs: Math.abs, hyp: (a, b) => Math.hypot(a, b), fact: n => n <= 1 ? 1 : n * fns.fact(n - 1) };
function evaluate(n) {
  switch (n.k) {
    case 'num': return n.v;
    case 'var': if (!(n.n in env)) throw new ReferenceError(n.n + ' undefined'); return env[n.n];
    case 'neg': return -evaluate(n.e);
    case 'assign': return env[n.n] = evaluate(n.e);
    case 'call': { const f = fns[n.f]; if (!f) throw new TypeError(n.f + ' is not a function'); return f(...n.args.map(evaluate)); }
    case 'bin': { const a = evaluate(n.l), b = evaluate(n.r); return { '+': a + b, '-': a - b, '*': a * b, '/': a / b, '%': a % b, '^': a ** b }[n.op]; }
  }
}
const show = n => n.k === 'num' ? String(n.v) : n.k === 'var' ? n.n : n.k === 'neg' ? `(-${show(n.e)})` : n.k === 'assign' ? `${n.n} = ${show(n.e)}` : n.k === 'call' ? `${n.f}(${n.args.map(show).join(', ')})` : `(${show(n.l)} ${n.op} ${show(n.r)})`;
const programs = ['1 + 2 * 3', '(1 + 2) * 3', '2 ^ 3 ^ 2', 'x = 10', 'y = x * 2 + 1', 'sqrt(x * x + y * y)', 'max(1, 5, 3) - min(4, 2)', 'fact(10) / fact(8)',
  '-x + -(-3)', '7 % 4 + 2 ** 10', 'hyp(3, 4) * pi', '1.5e3 / 3', 'z + 1', 'nope(1)', '(1 + 2', '1 + * 2', '3 $ 4', '1e400 - 1e400', '0.1 + 0.2'];
for (const p of programs) {
  try { const ast = parse(tokenize(p)); console.log(p.padEnd(26), '=>', show(ast).padEnd(34), '=', evaluate(ast)); }
  catch (e) { console.log(p.padEnd(26), '!!', e.name + ': ' + e.message); }
}
const freq = {}; for (const tok of tokenize('a = b + c * a - a / b')) freq[tok.v] = (freq[tok.v] || 0) + 1;
console.log(Object.entries(freq).sort((a, b) => b[1] - a[1] || String(a[0]).localeCompare(String(b[0]))).map(([k, v]) => `${k}:${v}`).join(' '));
