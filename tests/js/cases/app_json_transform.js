// JSON-heavy data transform: parse a dataset, aggregate, group, join, format a report
const raw = JSON.stringify({
  users: Array.from({ length: 40 }, (_, i) => ({ id: i + 1, name: `user${String(i + 1).padStart(2, '0')}`, country: ['US', 'DE', 'JP', 'BR', 'FR'][i % 5], born: 1960 + (i * 7) % 45, active: i % 3 !== 0 })),
  orders: Array.from({ length: 200 }, (_, i) => ({ oid: 1000 + i, uid: (i * 13) % 40 + 1, amount: Math.round(((i * 37) % 500 + 10) * 100) / 100, items: (i % 4) + 1, date: `2024-${String(i % 12 + 1).padStart(2, '0')}-${String(i % 28 + 1).padStart(2, '0')}` })),
});
const data = JSON.parse(raw, (k, v) => k === 'date' ? new Date(v + 'T00:00:00Z') : v);
const usersById = new Map(data.users.map(u => [u.id, u]));
const byCountry = {};
for (const o of data.orders) {
  const u = usersById.get(o.uid);
  const c = byCountry[u.country] ??= { orders: 0, revenue: 0, items: 0, buyers: new Set(), months: {} };
  c.orders++; c.revenue += o.amount; c.items += o.items; c.buyers.add(u.id);
  const mon = o.date.toISOString().slice(0, 7); c.months[mon] = (c.months[mon] || 0) + o.amount;
}
const report = Object.entries(byCountry).sort(([, a], [, b]) => b.revenue - a.revenue).map(([country, c]) => ({
  country, orders: c.orders, revenue: c.revenue.toFixed(2), avg: (c.revenue / c.orders).toFixed(2), items: c.items, buyers: c.buyers.size,
  bestMonth: Object.entries(c.months).reduce((best, cur) => cur[1] > best[1] ? cur : best)[0],
}));
console.log(JSON.stringify(report, null, 2));
const ageBuckets = data.users.reduce((acc, u) => { const decade = Math.floor((2024 - u.born) / 10) * 10; (acc[decade + 's'] ||= []).push(u.name); return acc; }, {});
console.log(JSON.stringify(ageBuckets));
const topSpenders = [...data.orders.reduce((m, o) => m.set(o.uid, (m.get(o.uid) || 0) + o.amount), new Map())]
  .sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, 5).map(([uid, amt]) => `${usersById.get(uid).name}=${amt.toFixed(2)}`);
console.log(topSpenders.join(', '));
const inactiveWithOrders = data.users.filter(u => !u.active && data.orders.some(o => o.uid === u.id)).map(u => u.id);
console.log(inactiveWithOrders.length, inactiveWithOrders.slice(0, 8));
const csv = ['oid,user,amount,date', ...data.orders.slice(0, 5).map(o => [o.oid, usersById.get(o.uid).name, o.amount, o.date.toISOString().slice(0, 10)].join(','))].join('\n');
console.log(csv);
const parsedCsv = csv.split('\n').slice(1).map(line => { const [oid, user, amount, date] = line.split(','); return { oid: +oid, user, amount: +amount, date }; });
console.log(JSON.stringify(parsedCsv[2]), parsedCsv.reduce((s, r) => s + r.amount, 0).toFixed(2));
const totals = data.orders.reduce((t, o) => ({ count: t.count + 1, sum: t.sum + o.amount, max: Math.max(t.max, o.amount), min: Math.min(t.min, o.amount) }), { count: 0, sum: 0, max: -Infinity, min: Infinity });
console.log(totals.count, totals.sum.toFixed(2), totals.max, totals.min, (totals.sum / totals.count).toFixed(4));
const deepCopy = structuredClone({ report: report.slice(0, 1), when: new Date(0), tags: new Set(['a']) });
console.log(deepCopy.report[0].country, deepCopy.when.toISOString(), [...deepCopy.tags]);
