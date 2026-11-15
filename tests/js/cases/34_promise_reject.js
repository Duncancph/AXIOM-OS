// regression: the reject function given to a Promise executor found the wrong hidden value and crashed Web
// (claude.ai: a request that timed out rejected fetch()'s promise)
var p = new Promise(function (resolve, reject) { Promise.resolve().then(function () { reject(new TypeError('Failed to fetch')); }); });
(async function () { try { await p; } catch (e) { console.log('caught ' + e.message); } })();
new Promise(function (ok, no) { no(1); }).catch(function (v) { console.log('sync reject ' + v); });
new Promise(function (a, b) { a(7); b(8); }).then(function (v) { console.log('first wins ' + v); });
Promise.all([Promise.resolve(1), new Promise(function (a, b) { b('x'); })]).catch(function (e) { console.log('all ' + e); });
Promise.allSettled([Promise.reject(2), 3]).then(function (r) { console.log('settled ' + JSON.stringify(r)); });
Promise.any([Promise.reject(1), Promise.resolve(5)]).then(function (v) { console.log('any ' + v); });
console.log('reject length ' + (function () { var r; new Promise(function (a, b) { r = b; }); return r.length; })());
