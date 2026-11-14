// A small reactive todo app on plain objects (observer pattern, computed values, undo history)
function reactive(target, onChange) {
  return new Proxy(target, {
    set(t, k, v) { const old = t[k]; t[k] = v; if (old !== v) onChange(k, old, v); return true; },
    deleteProperty(t, k) { delete t[k]; onChange(k, undefined, undefined); return true; },
  });
}
class Store {
  #listeners = new Set(); #history = [];
  constructor() { this.state = reactive({ todos: [], filter: 'all', nextId: 1 }, (k) => this.emit(k)); }
  subscribe(fn) { this.#listeners.add(fn); return () => this.#listeners.delete(fn); }
  emit(key) { for (const l of this.#listeners) l(key, this.state); }
  snapshot() { this.#history.push(JSON.stringify(this.state.todos)); }
  undo() { const prev = this.#history.pop(); if (prev !== undefined) this.state.todos = JSON.parse(prev); }
  add(text) { this.snapshot(); this.state.todos = [...this.state.todos, { id: this.state.nextId++, text, done: false, created: this.state.nextId * 1000 }]; }
  toggle(id) { this.snapshot(); this.state.todos = this.state.todos.map(t => t.id === id ? { ...t, done: !t.done } : t); }
  remove(id) { this.snapshot(); this.state.todos = this.state.todos.filter(t => t.id !== id); }
  get visible() { const f = this.state.filter; return this.state.todos.filter(t => f === 'all' || (f === 'done' ? t.done : !t.done)); }
  get stats() { const total = this.state.todos.length, done = this.state.todos.filter(t => t.done).length; return { total, done, left: total - done, pct: total ? Math.round(done / total * 100) : 0 }; }
}
const store = new Store();
const renders = [];
const unsub = store.subscribe((key, state) => renders.push(`${key}:${state.todos.length}`));
function render() {
  const lines = store.visible.map(t => `[${t.done ? 'x' : ' '}] #${t.id} ${t.text}`);
  const s = store.stats;
  return lines.concat(`-- ${s.left} left, ${s.done}/${s.total} done (${s.pct}%) filter=${store.state.filter}`).join('\n');
}
['Buy milk', 'Write engine', 'Test engine', 'Ship it'].forEach(t => store.add(t));
store.toggle(2); store.toggle(3);
console.log(render());
store.state.filter = 'done'; console.log(render());
store.state.filter = 'active'; console.log(render());
store.remove(1); store.state.filter = 'all'; console.log(render());
store.undo(); console.log(render());
store.undo(); store.undo(); console.log(render());
unsub(); store.add('silent');
console.log(renders.join(' '));
const byStatus = store.state.todos.reduce((acc, t) => ((acc[t.done ? 'done' : 'todo'] ||= []).push(t.text), acc), {});
console.log(JSON.stringify(byStatus));
const sorted = [...store.state.todos].sort((a, b) => a.text.localeCompare(b.text)).map(t => t.text);
console.log(sorted.join(', '));
const search = q => store.state.todos.filter(t => t.text.toLowerCase().includes(q.toLowerCase())).map(t => t.id);
console.log(search('ENGINE'), search('zzz'));
