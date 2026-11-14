// Promise/async pipeline: fake fetch with latency queue, retries, concurrency limit, async iteration
const events = [];
let tick = 0;
const timers = [];
function sleep(ms) { return new Promise(res => timers.push({ at: tick + ms, res })); }
async function runTimers() { while (timers.length) { timers.sort((a, b) => a.at - b.at); const t = timers.shift(); tick = t.at; t.res(); await null; await null; } }
const db = { '/users': [{ id: 1 }, { id: 2 }, { id: 3 }], '/user/1': { name: 'Ann' }, '/user/2': { name: 'Bob' }, '/user/3': { name: 'Cy' } };
let failures = { '/user/2': 2 };
async function fakeFetch(url) {
  await sleep(url.length);
  if (failures[url] > 0) { failures[url]--; throw new Error('503 ' + url); }
  if (!(url in db)) throw new Error('404 ' + url);
  return { ok: true, json: async () => JSON.parse(JSON.stringify(db[url])) };
}
async function withRetry(fn, tries = 3) {
  for (let i = 1; ; i++) {
    try { return await fn(); } catch (e) { events.push(`retry ${i}: ${e.message}`); if (i >= tries) throw e; await sleep(5 * i); }
  }
}
async function limit(tasks, n) {
  const results = []; let next = 0;
  async function worker(w) { while (next < tasks.length) { const i = next++; events.push(`w${w} start ${i}`); results[i] = await tasks[i](); events.push(`w${w} done ${i}`); } }
  await Promise.all(Array.from({ length: n }, (_, w) => worker(w)));
  return results;
}
async function* paginate(pages) { for (let p = 1; p <= pages; p++) { await sleep(1); yield { page: p, items: [p * 10, p * 10 + 1] }; } }
async function main() {
  const users = await (await fakeFetch('/users')).json();
  const tasks = users.map(u => () => withRetry(async () => (await fakeFetch('/user/' + u.id)).json()));
  const profiles = await limit(tasks, 2);
  events.push('profiles ' + profiles.map(p => p.name).join(','));
  try { await withRetry(() => fakeFetch('/missing'), 2); } catch (e) { events.push('final failure ' + e.message); }
  const settled = await Promise.allSettled([fakeFetch('/users'), fakeFetch('/nope')]);
  events.push('settled ' + settled.map(s => s.status).join(','));
  const raced = await Promise.race([sleep(50).then(() => 'slow'), sleep(10).then(() => 'fast')]);
  events.push('race ' + raced);
  let total = 0; for await (const pg of paginate(3)) { total += pg.items.reduce((a, b) => a + b); events.push('page ' + pg.page); }
  events.push('total ' + total);
  return 'main done at tick ' + tick;
}
main().then(r => events.push(r)).catch(e => events.push('main failed ' + e));
runTimers().then(() => console.log(events.join('\n')));
