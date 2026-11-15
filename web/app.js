'use strict';
// CHRONOS dashboard. Everything shown comes from the engine (WebSocket snapshots) or benchmark result files.
const $ = (s, r = document) => r.querySelector(s), $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fmt = (n, d = 0) => { n = +n; if (!isFinite(n)) return '–'; const a = Math.abs(n);
  return a >= 1e9 ? (n / 1e9).toFixed(2) + 'B' : a >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : a >= 1e4 ? (n / 1e3).toFixed(1) + 'K' : n.toLocaleString(undefined, { maximumFractionDigits: d }); };
const ns = n => !isFinite(n) ? '–' : n >= 1e6 ? (n / 1e6).toFixed(2) + ' ms' : n >= 1e3 ? (n / 1e3).toFixed(2) + ' µs' : Math.round(n) + ' ns';
const mb = b => (b / 1048576).toFixed(b >= 1e8 ? 0 : 1) + ' MB';
const COLORS = { std: '#5a5a5a', tree: '#9a9a9a', array: '#eeeeee', adaptive: '#ff6a2b' };
const NAMES = { std: 'STL baseline', tree: 'Custom · tree', array: 'Custom · array', adaptive: 'CHRONOS-ADAPT' };
const WLS = ['uniform', 'concentrated', 'sparse', 'high_cancel', 'bursty', 'mixed'];

async function api(path, body) {
  const r = await fetch(path, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({ error: 'bad response' }));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

// ------------------------------------------------------------------ charts (canvas, no libs)
function prep(cv) { const d = devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight; cv.width = w * d; cv.height = h * d;
  const g = cv.getContext('2d'); g.scale(d, d); g.clearRect(0, 0, w, h); g.font = '10px ui-monospace, Menlo, monospace'; return { g, w, h }; }
function axes(g, w, h, max, L, B, fmtY) { g.strokeStyle = '#202020'; g.fillStyle = '#7e7e7e'; g.lineWidth = 1;
  for (let i = 0; i <= 4; i++) { const y = 8 + (h - B - 8) * i / 4; g.beginPath(); g.moveTo(L, y); g.lineTo(w - 6, y); g.stroke(); g.fillText(fmtY(max * (1 - i / 4)), 2, y + 3); } }
function hoverable(cv) { if (cv._hv) return; cv._hv = 1;
  cv.addEventListener('mousemove', e => { const r = cv.getBoundingClientRect(); cv._hx = e.clientX - r.left; cv._hy = e.clientY - r.top; cv._redraw && cv._redraw(); });
  cv.addEventListener('mouseleave', () => { cv._hx = null; cv._redraw && cv._redraw(); }); }
function tip(g, w, x, y, lines) { g.font = '11px ui-monospace, Menlo, monospace'; const tw = Math.max(...lines.map(l => g.measureText(l).width)) + 16, th = lines.length * 15 + 10, tx = Math.min(w - tw - 4, Math.max(4, x + 12)), ty = Math.max(4, y - th - 8);
  g.fillStyle = '#eeeeee'; g.fillRect(tx, ty, tw, th); g.fillStyle = '#000'; lines.forEach((l, i) => g.fillText(l, tx + 8, ty + 17 + i * 15)); }
function lineChart(cv, sets, fmtY = fmt) {
  const draw = () => { const { g, w, h } = prep(cv), L = 46, B = 14, n = Math.max(...sets.map(s => s.data.length), 2);
    const max = Math.max(1e-9, ...sets.flatMap(s => s.data)) * 1.1; axes(g, w, h, max, L, B, fmtY);
    sets.forEach(s => { g.strokeStyle = s.color; g.lineWidth = 1.6; g.beginPath();
      s.data.forEach((v, i) => { const x = L + (w - L - 6) * i / (n - 1), y = 8 + (h - B - 8) * (1 - v / max); i ? g.lineTo(x, y) : g.moveTo(x, y); }); g.stroke(); });
    if (cv._hx != null && cv._hx > L) { const i = Math.max(0, Math.min(n - 1, Math.round((cv._hx - L) / (w - L - 6) * (n - 1)))), x = L + (w - L - 6) * i / (n - 1);
      g.strokeStyle = '#5a5a5a'; g.beginPath(); g.moveTo(x, 8); g.lineTo(x, h - B); g.stroke();
      sets.forEach(s => { if (s.data[i] === undefined) return; g.fillStyle = s.color; g.fillRect(x - 3, 8 + (h - B - 8) * (1 - s.data[i] / max) - 3, 6, 6); });
      tip(g, w, x, cv._hy, sets.filter(s => s.data[i] !== undefined).map(s => (s.name ? s.name + ' ' : '') + fmtY(s.data[i]))); } };
  cv._redraw = draw; hoverable(cv); draw();
}
function barChart(cv, groups, sers, fmtY = fmt) { // groups: [{label, vals:{key:value}}]
  const draw = () => { const { g, w, h } = prep(cv), L = 50, B = 18, max = Math.max(1e-9, ...groups.flatMap(x => sers.map(s => x.vals[s] || 0))) * 1.1; axes(g, w, h, max, L, B, fmtY);
    const gw = (w - L - 6) / groups.length, bw = Math.min(26, gw * 0.8 / sers.length); let hit = null;
    groups.forEach((x, i) => { const x0 = L + gw * i + (gw - bw * sers.length) / 2;
      sers.forEach((s, k) => { const v = x.vals[s] || 0, bh = (h - B - 8) * v / max, bx = x0 + bw * k, by = h - B - bh, on = cv._hx != null && cv._hx >= bx && cv._hx < bx + bw && cv._hy >= by - 4 && cv._hy <= h - B;
        g.fillStyle = COLORS[s] || '#eeeeee'; g.globalAlpha = cv._hx != null && !on ? .55 : 1; g.fillRect(bx, by, bw - 2, bh); g.globalAlpha = 1; if (on) hit = { s, v, label: x.label, bx, by }; });
      g.fillStyle = '#a0a0a0'; g.textAlign = 'center'; g.fillText(x.label, L + gw * i + gw / 2, h - 4); g.textAlign = 'left'; });
    if (hit) tip(g, w, hit.bx + bw / 2, hit.by, [(NAMES[hit.s] || hit.label) + ' · ' + hit.label, fmtY(hit.v)]); };
  cv._redraw = draw; hoverable(cv); draw();
}
function spark(cv, data, color) { const d = devicePixelRatio || 1, w = cv.clientWidth, h = cv.clientHeight; cv.width = w * d; cv.height = h * d; const g = cv.getContext('2d'); g.scale(d, d); if (data.length < 2) return;
  const mx = Math.max(...data), mn = Math.min(...data), r = mx - mn || 1; g.strokeStyle = color; g.lineWidth = 1.4; g.beginPath(); data.forEach((v, i) => { const x = w * i / (data.length - 1), y = h - 2 - (h - 4) * (v - mn) / r; i ? g.lineTo(x, y) : g.moveTo(x, y); }); g.stroke(); }
const legend = keys => `<div class="legend">${keys.map(k => `<span><i style="background:${COLORS[k] || k}"></i>${esc(NAMES[k] || k)}</span>`).join('')}</div>`;

// ------------------------------------------------------------------ state
let S = null, page = 'overview', trades = [], tradeSort = { k: 0, dir: -1 }, tradeFilter = {};
const ser = { ops: [], avg: [], p99: [], mem: [], recv: [] };
const push = (a, v) => { a.push(v); if (a.length > 240) a.shift(); };

function connect() {
  const ws = new WebSocket(`ws://${location.host}/ws`), pc = $('#pConn');
  ws.onopen = () => { pc.className = 'pill ok'; pc.innerHTML = '<i></i>live · ws'; };
  ws.onclose = () => { pc.className = 'pill'; pc.innerHTML = '<i></i>disconnected'; setTimeout(connect, 1500); };
  ws.onmessage = e => { S = JSON.parse(e.data); onSnap(); };
}
function onSnap() {
  const landing = document.body.classList.contains('is-landing'); if (landing && window.Landing) Landing.snap(S);
  const st = S.stats;
  push(ser.ops, st.opsPerSec); push(ser.avg, st.avgNs); push(ser.p99, st.p99Ns); push(ser.mem, st.heapBytes); push(ser.recv, st.received);
  for (const t of S.trades || []) trades.push(t); if (trades.length > 5000) trades.splice(0, trades.length - 5000);
  $('#pMode').textContent = 'mode ' + S.mode; const ps = $('#pSim'); ps.textContent = S.sim ? `sim ${S.simKind} @ ${fmt(S.rate)}/s` : 'sim off'; ps.className = 'pill' + (S.sim ? ' live' : '');
  if (!landing) (pages[page].update || (() => {}))(S);
}

// ------------------------------------------------------------------ pages
const kpi = (id, label, sub = '', sp = '') => `<div class="card kpi"><h3>${label}</h3><div class="v" id="${id}">–</div><div class="s" id="${id}s">${sub}</div>${sp ? `<canvas class="spark" data-s="${sp}"></canvas>` : ''}</div>`;
const set = (id, v, cls) => { const e = document.getElementById(id); if (e) { e.textContent = v; if (cls !== undefined) e.className = (e.className.split(' ').filter(c => c === 'v')).join(' ') + (cls ? ' ' + cls : ''); } };

const pages = {
  overview: { title: 'Live overview',
    html: `<div class="demo" id="demo"><div><b>Engine idle.</b> Start the synthetic market to watch orders match, the book move, and CHRONOS-ADAPT switch strategies.</div><button class="go" id="demoGo">▶ Start live demo</button></div><div class="grid g5">${kpi('k1', 'Engine status')}${kpi('k2', 'Orders received', '', 'recv')}${kpi('k3', 'Orders matched', 'incoming orders with ≥1 fill')}${kpi('k4', 'Active buy orders')}${kpi('k5', 'Active sell orders')}
      ${kpi('k6', 'Total trades')}${kpi('k7', 'Throughput', 'commands/s, live sim (not exchange throughput)', 'ops')}${kpi('k8', 'Avg latency', 'per command, last 32K', 'avg')}${kpi('k9', 'p99 latency', '', 'p99')}${kpi('k10', 'Memory', 'process heap in use', 'mem')}</div>
      <div class="grid g2 gap"><div class="card"><h3>Throughput (cmds/s)</h3><canvas id="c1"></canvas></div><div class="card"><h3>Latency · avg vs p99</h3><div class="legend"><span><i style="background:#eeeeee"></i>avg</span><span><i style="background:#ff6a2b"></i>p99</span></div><canvas id="c2"></canvas></div></div>
      <p class="note">Go to <b>Orders</b> to start the synthetic simulator or submit orders by hand. Latency = wall time of <code>Engine::process</code> measured around each call (includes ~20–40 ns of clock reads).</p>`,
    update(s) { const t = s.stats; $('#demo').classList.toggle('show', !s.sim && t.received === 0); const dg = $('#demoGo'); if (dg) dg.onclick = async () => { dg.disabled = true; try { await api('/api/sim', { action: 'start', workload: 'mixed', rate: 50000 }); } catch (e) {} dg.disabled = false; };
      set('k1', s.sim ? 'SIMULATING' : 'IDLE', s.sim ? 'ember' : 'dim'); set('k1s', `${s.mode} · ${s.adapt.usingArray ? 'array' : 'tree'} index`);
      set('k2', fmt(t.received)); set('k3', fmt(t.matched)); set('k4', fmt(t.activeBuy), 'buy'); set('k5', fmt(t.activeSell), 'sell'); set('k6', fmt(t.trades)); set('k7', fmt(t.opsPerSec), 'ember');
      set('k8', ns(t.avgNs)); set('k9', ns(t.p99Ns)); set('k10', mb(t.heapBytes)); set('k10s', `engine structures ≈ ${mb(t.engineBytes)}`);
      lineChart($('#c1'), [{ data: ser.ops, color: '#ff6a2b' }]); lineChart($('#c2'), [{ data: ser.avg, color: '#eeeeee', name: 'avg' }, { data: ser.p99, color: '#ff6a2b', name: 'p99' }], ns);
      $$('.spark').forEach(c => spark(c, ser[c.dataset.s] || [], '#a0a0a0')); } },

  book: { title: 'Live order book', prev: {},
    html: `<div class="grid g2"><div class="card bid"><h3 class="buy">Buy side · bids</h3><table class="tbl"><thead><tr><th>Price</th><th>Quantity</th><th>Orders</th><th>Cumulative</th></tr></thead><tbody id="bids"></tbody></table></div>
      <div class="card ask"><h3 class="sell">Sell side · asks</h3><table class="tbl"><thead><tr><th>Price</th><th>Quantity</th><th>Orders</th><th>Cumulative</th></tr></thead><tbody id="asks"></tbody></table></div></div>
      <div class="spread" id="spr">–</div><p class="note">Top 14 price levels per side. Best bid / best ask highlighted. Pushed over WebSocket every 250 ms as one batched snapshot.</p>`,
    update(s) { const side = (rows, cls) => { const mx = rows.length ? rows[rows.length - 1][3] : 1, pv = pages.book.prev[cls] || {}, nx = {}; const html = rows.map((r, i) => { nx[r[0]] = r[1]; const o = pv[r[0]], f = o === undefined ? (Object.keys(pv).length ? 'up' : '') : r[1] > o ? 'up' : r[1] < o ? 'dn' : ''; return `<tr class="${i ? '' : 'best'}"><td class="${cls}">${r[0]}</td><td class="${f}">${fmt(r[1])}</td><td>${r[2]}</td><td class="bar" style="--w:${100 * r[3] / mx}%"><span>${fmt(r[3])}</span></td></tr>`; }).join(''); pages.book.prev[cls] = nx; return html; };
      $('#bids').innerHTML = side(s.book.bids, 'buy'); $('#asks').innerHTML = side(s.book.asks, 'sell');
      const b = s.book.bids[0], a = s.book.asks[0]; $('#spr').innerHTML = b && a ? `best bid <b class="buy">${b[0]}</b> · best ask <b class="sell">${a[0]}</b> · spread <b>${a[0] - b[0]}</b> ticks` : 'book is empty — start the simulator on the Orders page'; } },

  trades: { title: 'Trade history',
    html: `<div class="card solo"><div class="row" style="margin-bottom:10px"><label>Min qty<input id="fq" type="number" min="0" placeholder="0"></label><label>Min price<input id="fpl" type="number" placeholder=""></label><label>Max price<input id="fph" type="number" placeholder=""></label><label>Order id (buy or sell)<input id="fid" type="number" min="1" placeholder=""></label></div>
      <div class="scroll"><table class="tbl"><thead><tr id="th"></tr></thead><tbody id="tb"></tbody></table></div><div class="note" id="tn"></div></div>`,
    init() { const cols = ['Trade ID', 'Buy order', 'Sell order', 'Price', 'Qty', 'Time'];
      $('#th').innerHTML = cols.map((c, i) => `<th class="sort" data-k="${i}">${c}</th>`).join(''); $('#th').onclick = e => { const k = e.target.dataset.k; if (k === undefined) return; tradeSort = { k: +k, dir: tradeSort.k === +k ? -tradeSort.dir : -1 }; pages.trades.update(); };
      ['fq', 'fpl', 'fph', 'fid'].forEach(i => $('#' + i).oninput = () => { tradeFilter[i] = $('#' + i).value; pages.trades.update(); });
      api('/api/trades?limit=2000').then(a => { const seen = new Set(trades.map(t => t[0])); trades = a.filter(t => !seen.has(t[0])).concat(trades); pages.trades.update(); }).catch(() => {}); },
    update() { const f = tradeFilter, num = k => f[k] === undefined || f[k] === '' ? null : +f[k];
      let r = trades.filter(t => (num('fq') === null || t[4] >= num('fq')) && (num('fpl') === null || t[3] >= num('fpl')) && (num('fph') === null || t[3] <= num('fph')) && (num('fid') === null || t[1] === num('fid') || t[2] === num('fid')));
      r = r.slice().sort((a, b) => (a[tradeSort.k] - b[tradeSort.k]) * tradeSort.dir); const shown = r.slice(0, 300);
      $('#tb').innerHTML = shown.map(t => `<tr><td>${t[0]}</td><td class="buy">${t[1]}</td><td class="sell">${t[2]}</td><td>${t[3]}</td><td>${fmt(t[4])}</td><td>${new Date(t[5]).toLocaleTimeString()}.${String(t[5] % 1000).padStart(3, '0')}</td></tr>`).join('');
      $('#tn').textContent = `showing ${shown.length} of ${r.length} matching trades (client keeps the last 5000)`; } },

  orders: { title: 'Order management',
    html: `<div class="grid g3"><div class="card"><h3>Submit order</h3><form class="f" id="fo"><div class="row"><label>Side<select name="side"><option value="buy">Buy</option><option value="sell">Sell</option></select></label><label>Type<select name="type"><option value="limit">Limit</option><option value="market">Market</option></select></label></div>
        <div class="row"><label>Price (ticks 0–32767)<input name="price" type="number" step="1" min="0" max="32767" value="16384"></label><label>Quantity<input name="qty" type="number" step="1" min="1" max="1000000000000" value="100" required></label></div>
        <label>Order id (optional)<input name="id" type="number" step="1" min="1" placeholder="auto"></label><div class="row"><button class="b" type="submit" name="b" value="buy">Submit</button></div></form></div>
      <div class="card"><h3>Cancel / modify</h3><form class="f" id="fc"><label>Order id<input name="id" type="number" step="1" min="1" required></label><div class="row"><button class="s" type="submit" value="cancel">Cancel order</button></div></form><hr>
        <form class="f" id="fm"><div class="row"><label>New price<input name="price" type="number" step="1" min="0" max="32767" required></label><label>New qty<input name="qty" type="number" step="1" min="1" required></label></div><label>Order id<input name="id" type="number" step="1" min="1" required></label><button type="submit">Modify (cancel + replace, loses time priority)</button></form></div>
      <div class="card"><h3>Simulator &amp; engine</h3><form class="f" id="fs"><div class="row"><label>Index strategy<select name="mode" id="smode"><option value="adaptive">CHRONOS-ADAPT</option><option value="tree">Fixed tree</option><option value="array">Fixed array</option></select></label><label>Workload<select name="workload">${WLS.map(w => `<option ${w === 'mixed' ? 'selected' : ''}>${w}</option>`).join('')}</select></label></div>
        <div class="row"><label>Orders / sec<input name="rate" type="number" min="1" max="2000000" value="50000"></label><label>Cancel ratio<input name="cancel" type="number" step="0.05" min="0" max="0.95" placeholder="preset"></label><label>Seed<input name="seed" type="number" value="42"></label></div>
        <div class="row"><button type="submit" value="start" class="b">Start (resets book)</button><button type="button" id="sstop">Stop</button></div></form><p class="note">Synthetic orders only. Changing strategy or starting a workload resets the book.</p></div></div>
      <div class="card solo gap"><h3>Engine responses</h3><div class="log" id="log"></div></div>`,
    init() { const log = (m, c) => { const l = $('#log'); l.insertAdjacentHTML('afterbegin', `<div class="${c || ''}">${esc(m)}</div>`); };
      const num = (f, k) => { const v = f.get(k); return v === '' || v === null ? undefined : Number(v); };
      const go = async (path, body) => { try { const r = await api(path, body); log(path + ' → ' + JSON.stringify(r), 'okc'); toast(r.status ? `${path.slice(5)} · ${r.status}${r.trades && r.trades.length ? ' · ' + r.trades.length + ' trade(s)' : ''}` : path.slice(5) + ' · ok', /^rej/.test(r.status || '') ? 'err' : ''); } catch (e) { log(path + ' ✗ ' + e.message, 'err'); toast(e.message, 'err'); } };
      const typeSel = $('#fo [name=type]'), price = $('#fo [name=price]'); typeSel.onchange = () => { price.disabled = typeSel.value === 'market'; price.required = !price.disabled; }; typeSel.onchange();
      $('#fo').onsubmit = e => { e.preventDefault(); const f = new FormData(e.target), b = { side: f.get('side'), type: f.get('type'), qty: num(f, 'qty') }; if (b.type === 'limit') b.price = num(f, 'price'); const id = num(f, 'id'); if (id) b.id = id;
        if (!Number.isInteger(b.qty) || b.qty < 1) return log('quantity must be a positive integer', 'err'); if (b.type === 'limit' && (!Number.isInteger(b.price) || b.price < 0 || b.price > 32767)) return log('price must be an integer in 0..32767', 'err'); go('/api/order', b); };
      $('#fc').onsubmit = e => { e.preventDefault(); go('/api/cancel', { id: num(new FormData(e.target), 'id') }); };
      $('#fm').onsubmit = e => { e.preventDefault(); const f = new FormData(e.target); go('/api/modify', { id: num(f, 'id'), price: num(f, 'price'), qty: num(f, 'qty') }); };
      $('#fs').onsubmit = async e => { e.preventDefault(); const f = new FormData(e.target); await go('/api/engine', { mode: f.get('mode') }); const b = { action: 'start', workload: f.get('workload'), rate: num(f, 'rate'), seed: num(f, 'seed') }; const c = num(f, 'cancel'); if (c !== undefined) b.cancel = c; go('/api/sim', b); };
      $('#sstop').onclick = () => go('/api/sim', { action: 'stop' }); } },

  analytics: { title: 'Performance analytics',
    html: `<div class="grid g2"><div class="card"><h3>Throughput over time (live)</h3><canvas id="a1"></canvas></div><div class="card"><h3>Orders processed over time (live)</h3><canvas id="a2"></canvas></div>
      <div class="card"><h3>Memory · process heap in use</h3><canvas id="a3"></canvas></div><div class="card"><h3>Live latency distribution (last 32K commands)</h3><canvas id="a4"></canvas></div></div>
      <div class="card solo gap"><h3>Engine versions · offline benchmark</h3><div class="row" style="margin-bottom:8px"><label>Result file<select id="af"></select></label><label>Metric<select id="am"><option value="ops">Throughput (Mops/s)</option><option value="p99">p99 latency (ns)</option><option value="p999">p99.9 latency (ns)</option><option value="mem">Heap (MB)</option></select></label></div><div id="al"></div><canvas id="a5" style="height:240px"></canvas><div class="note" id="an"></div></div>`,
    init() { loadBenchList($('#af'), () => drawCompare()); $('#am').onchange = drawCompare; $('#af').onchange = drawCompare; },
    update(s) { lineChart($('#a1'), [{ data: ser.ops, color: '#ff6a2b' }]); lineChart($('#a2'), [{ data: ser.recv, color: '#eeeeee' }]); lineChart($('#a3'), [{ data: ser.mem, color: '#b4b4b4' }], mb);
      const t = s.stats; barChart($('#a4'), [{ label: 'avg', vals: { x: t.avgNs } }, { label: 'p50', vals: { x: t.p50Ns } }, { label: 'p99', vals: { x: t.p99Ns } }, { label: 'p99.9', vals: { x: t.p999Ns } }], ['x'], ns); } },

  adapt: { title: 'CHRONOS-ADAPT monitor',
    html: `<div class="grid g3"><div class="card"><h3>Active indexing strategy</h3><span class="badge tree" id="ab">–</span><div class="s dim" id="am0" style="margin-top:8px"></div></div>
      <div class="card" style="grid-column:span 2"><h3>Why this representation</h3><div id="ar" class="reason"></div><div class="note" id="ac"></div></div></div>
      <div class="grid g5 gap">${kpi('m1', 'Active price levels')}${kpi('m2', 'Level density', 'levels / price span')}${kpi('m3', 'Insertion rate', 'resting inserts / op')}${kpi('m4', 'Cancellation rate', 'cancels / op')}${kpi('m5', 'Level churn', 'level create+erase / op')}
      ${kpi('m6', 'Best-price lookups', 'per op')}${kpi('m7', 'Recent latency', 'ns/op, last batch')}${kpi('m8', 'Index + book memory')}${kpi('m9', 'Adaptation events')}${kpi('m10', 'Last migration')}</div>
      <div class="card solo gap"><h3>Adaptation events (most recent 30)</h3><div class="scroll"><table class="tbl"><thead><tr><th>At op</th><th>Direction</th><th>Duration</th><th>Orders moved</th><th>Levels</th><th>Mem before</th><th>Mem after</th><th style="text-align:left">Reason</th></tr></thead><tbody id="ae"></tbody></table></div>
      <div class="note">Migration happens only between commands, at a batch boundary: build destination → relink + validate every order (price order, FIFO seq, aggregates) → swap. Order nodes are never copied, so timestamps and sequence numbers cannot change.</div></div>`,
    update(s) { const a = s.adapt, adaptive = s.mode === 'adaptive'; const b = $('#ab'); b.textContent = a.usingArray ? 'ARRAY + BITMAP' : 'TREE (std::map)'; b.className = 'badge ' + (a.usingArray ? 'array' : 'tree');
      $('#am0').textContent = adaptive ? 'adaptive: switches at batch boundaries' : `fixed strategy (${s.mode}) — adaptation disabled`;
      $('#ar').textContent = adaptive ? a.reason : 'Fixed-strategy mode: no decisions are made.'; const c = a.cfg; $('#ac').textContent = c.policy === 1 ? `policy v1 (density): batch=${c.batch} ops · array if density ≥ ${c.denseHi}, levels ≥ ${c.minLevels}` : `policy v2 (level count): batch=${c.batch} ops · array if levels ≥ ${c.levelsHi} and churn ≥ ${c.minChurn} and array fits ${c.memBudgetMB} MB · back to tree below ${c.levelsLo} levels · ${c.confirm} consecutive batches required`;
      set('m1', fmt(a.levels)); set('m2', (+a.density).toFixed(4)); set('m3', (+a.insertRate).toFixed(3)); set('m4', (+a.cancelRate).toFixed(3)); set('m5', (+a.churn).toFixed(3)); set('m6', (+a.bestRate).toFixed(2)); set('m7', ns(a.nsPerOp)); set('m8', mb(a.bytes));
      set('m9', a.nEvents); const e = a.events[a.events.length - 1]; set('m10', e ? e.ms.toFixed(2) + ' ms' : '–'); set('m10s', e ? `${fmt(e.orders)} orders, ${(e.toArray ? 'tree→array' : 'array→tree')}` : 'none yet');
      $('#ae').innerHTML = a.events.slice().reverse().map(e => `<tr><td>${fmt(e.op)}</td><td class="${e.toArray ? 'sys' : 'amber'}">${e.toArray ? 'tree → array' : 'array → tree'}</td><td>${e.ms.toFixed(2)} ms</td><td>${fmt(e.orders)}</td><td>${fmt(e.levels)}</td><td>${mb(e.before)}</td><td>${mb(e.after)}</td><td style="text-align:left" class="dim">${esc(e.why)}</td></tr>`).join('') || '<tr><td colspan="8" class="dim" style="text-align:left">no migrations yet</td></tr>'; } },

  lab: { title: 'Benchmark lab',
    html: `<div class="grid g2"><div class="card"><h3>Run a benchmark (separate process: ./build/chronos_bench)</h3><form class="f" id="bf"><div class="row"><label>Workload<select name="workload"><option value="all">all six</option>${WLS.map(w => `<option>${w}</option>`).join('')}</select></label><label>Orders<input name="n" type="number" min="1000" max="5000000" step="1000" value="200000"></label><label>Trials<input name="trials" type="number" min="1" max="10" value="3"></label><label>Cancel % (blank = preset)<input name="cancel" type="number" min="0" max="95" step="5"></label></div>
        <div class="chk">${['std', 'tree', 'array', 'adaptive'].map(k => `<label><input type="checkbox" name="s_${k}" checked><i style="width:9px;height:9px;background:${COLORS[k]};display:inline-block"></i>${NAMES[k]}</label>`).join('')}</div>
        <div class="row"><button type="submit" class="b" id="bgo">Start benchmark</button></div><div class="prog"><div id="bp"></div></div><div class="note" id="bs">idle. The live simulator is paused during a run so it cannot skew timings.</div></form></div>
      <div class="card"><h3>Results</h3><div class="row"><label>File<select id="bfile"></select></label><label>Compare with<select id="bcmp"><option value="">(none)</option></select></label><label>Metric<select id="bmet"><option value="ops">Throughput</option><option value="p99">p99</option><option value="p999">p99.9</option><option value="mem">Heap</option></select></label></div><div class="row" style="margin-top:8px"><button type="button" id="bjson">Export JSON</button><button type="button" id="bcsv">Export CSV</button></div><div class="note" id="bcfg"></div></div></div>
      <div class="card solo gap"><h3>Chart</h3><div id="bl"></div><canvas id="bc" style="height:250px"></canvas></div>
      <div class="card solo gap"><h3>Table · medians over trials · latencies are per-command wall time</h3><div class="scroll"><table class="tbl"><thead><tr><th>Workload</th><th>Strategy</th><th>Mops/s</th><th>avg</th><th>p50</th><th>p95</th><th>p99</th><th>p99.9</th><th>max</th><th>Heap MB</th><th>B/active</th><th>Allocs</th><th>Migr.</th><th>Ratio</th><th>OK</th></tr></thead><tbody id="bt"></tbody></table></div><div class="note" id="bn"></div></div>`,
    init() { loadBenchList($('#bfile'), () => drawLab()); const cmp = $('#bcmp'); fetch('/api/bench/list').then(r => r.json()).then(l => { l.sort().forEach(f => cmp.insertAdjacentHTML('beforeend', `<option>${esc(f)}</option>`)); });
      ['bfile', 'bcmp', 'bmet'].forEach(i => $('#' + i).onchange = drawLab); $('#bjson').onclick = () => download('chronos_bench.json', JSON.stringify(lab.a, null, 1), 'application/json');
      $('#bcsv').onclick = () => download('chronos_bench.csv', 'workload,strategy,ops_per_sec,avg_ns,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,heap_mb,allocs,migrations\n' + lab.a.results.map(r => [r.workload, r.strategy, r.opsPerSec, r.lat.avg, r.lat.p50, r.lat.p95, r.lat.p99, r.lat.p999, r.lat.max, r.heapMB, r.allocs, r.migrations].join(',')).join('\n'), 'text/csv');
      $('#bf').onsubmit = async e => { e.preventDefault(); const f = new FormData(e.target), b = { workload: f.get('workload'), n: +f.get('n'), trials: +f.get('trials'), strategies: ['std', 'tree', 'array', 'adaptive'].filter(k => f.get('s_' + k)).join(',') }; if (f.get('cancel') !== '') b.cancel = +f.get('cancel') / 100;
        if (!b.strategies) return set('bs', 'select at least one strategy', 'err'); try { await api('/api/bench/run', b); pollBench(); } catch (er) { set('bs', er.message, 'err'); } };
      pollBench(true); } },
};

// ------------------------------------------------------------------ benchmark helpers
const lab = { a: null, b: null };
function download(name, text, type) { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([text], { type })); a.download = name; a.click(); URL.revokeObjectURL(a.href); }
async function loadBenchList(sel, cb) { try { const l = (await api('/api/bench/list')).sort(); sel.innerHTML = l.map(f => `<option>${esc(f)}</option>`).join('') || '<option value="">no results yet — run chronos_bench</option>'; cb(); } catch (e) { sel.innerHTML = '<option value="">unavailable</option>'; } }
async function loadFile(f) { return f ? api('/api/bench/results?file=' + encodeURIComponent(f)) : null; }
const metric = (r, m) => m === 'ops' ? r.opsPerSec / 1e6 : m === 'p99' ? r.lat.p99 : m === 'p999' ? r.lat.p999 : r.heapMB;
const metricFmt = m => m === 'ops' ? v => v.toFixed(1) : m === 'mem' ? v => v.toFixed(0) : ns;
function strategiesOf(d) { return ['std', 'tree', 'array', 'adaptive'].filter(k => d.results.some(r => r.strategy === k)); }
function groupsOf(d, m) { const w = [...new Set(d.results.map(r => r.workload))]; return w.map(x => ({ label: x.replace('_', ' '), vals: Object.fromEntries(d.results.filter(r => r.workload === x).map(r => [r.strategy, metric(r, m)])) })); }
async function drawCompare() { const f = $('#af').value; if (!f) return; try { const d = await loadFile(f), m = $('#am').value, sers = strategiesOf(d);
  $('#al').innerHTML = legend(sers); barChart($('#a5'), groupsOf(d, m), sers, metricFmt(m)); $('#an').textContent = `${f}: n=${d.config.n}, seed=${d.config.seed}, ${d.config.trials} trials (+${d.config.warmup} warm-up), median. ${d.env.note}. ${d.env.compiler}`; } catch (e) { $('#an').textContent = e.message; } }
async function drawLab() { try { lab.a = await loadFile($('#bfile').value); lab.b = await loadFile($('#bcmp').value); } catch (e) { return set('bn', e.message); } if (!lab.a) return;
  const m = $('#bmet').value, sers = strategiesOf(lab.a); $('#bl').innerHTML = legend(sers); barChart($('#bc'), groupsOf(lab.a, m), sers, metricFmt(m));
  const c = lab.a.config; $('#bcfg').textContent = `n=${c.n} seed=${c.seed} trials=${c.trials} band=[${c.band}] policy=${c.policy === undefined ? 'v1' : 'v' + c.policy} · ${lab.a.env.compiler}`;
  const base = {}; lab.a.results.forEach(r => { if (r.strategy === 'tree') base[r.workload] = r.opsPerSec; });
  $('#bt').innerHTML = lab.a.results.map(r => { const o = lab.b && lab.b.results.find(x => x.workload === r.workload && x.strategy === r.strategy); const ratio = o ? (r.opsPerSec / o.opsPerSec).toFixed(2) + '× vs ' + $('#bcmp').value.slice(0, 14) : base[r.workload] ? (r.opsPerSec / base[r.workload]).toFixed(2) + '× vs tree' : '';
    return `<tr><td class="wl">${esc(r.workload)}</td><td class="wl" style="color:${COLORS[r.strategy]}">${esc(NAMES[r.strategy])}</td><td>${(r.opsPerSec / 1e6).toFixed(2)}</td><td>${ns(r.lat.avg)}</td><td>${ns(r.lat.p50)}</td><td>${ns(r.lat.p95)}</td><td>${ns(r.lat.p99)}</td><td>${ns(r.lat.p999)}</td><td>${ns(r.lat.max)}</td><td>${r.heapMB.toFixed(1)}</td><td>${fmt(r.bytesPerActive)}</td><td>${fmt(r.allocs)}</td><td>${r.migrations}</td><td>${ratio}</td><td class="${r.consistent ? 'buy' : 'sell'}">${r.consistent ? '✓' : '✗'}</td></tr>`; }).join('');
  $('#bn').textContent = '✓ = every strategy produced the identical trade stream (hash) and final active-order count for that workload. Max latency includes pool growth, page faults and migrations. Simulated in-process replay; not real exchange throughput.'; }
let polling = false;
async function pollBench(quiet) { if (polling) return; polling = true; try { for (;;) { const s = await api('/api/bench/status'); const p = $('#bp'); if (!p) break; p.style.transform = 'scaleX(' + (s.total ? s.done / s.total : 0) + ')';
  if (s.running) { set('bs', `running ${s.done}/${s.total} · ${s.label}`); await new Promise(r => setTimeout(r, 700)); continue; }
  if (s.error) set('bs', s.error, 'err'); else if (s.file) { set('bs', 'finished → ' + s.file, 'okc'); await loadBenchList($('#bfile'), () => {}); $('#bfile').value = s.file; drawLab(); } else if (!quiet) set('bs', 'idle'); break; } } finally { polling = false; } }

// ------------------------------------------------------------------ router
const nav = [['overview', 'Live overview'], ['book', 'Order book'], ['trades', 'Trade history'], ['orders', 'Order management'], ['analytics', 'Performance'], ['adapt', 'CHRONOS-ADAPT'], ['lab', 'Benchmark lab']];
$('#nav').innerHTML = nav.map(([k, t], i) => `<a href="#${k}" data-k="${k}"><span class="n">0${i + 1}</span>${t}</a>`).join('');
addEventListener('keydown', e => { if (e.metaKey || e.ctrlKey || e.altKey || /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName)) return; const i = +e.key - 1; if (i >= 0 && i < nav.length) location.hash = nav[i][0]; });
function route() { const h = location.hash.slice(1), home = h === '' || h === 'home'; document.body.classList.toggle('is-landing', home);
  if (home) { window.Landing && Landing.enter(); return; } if (['how', 'res', 'safe'].includes(h)) { document.body.classList.add('is-landing'); return; }
  window.Landing && Landing.leave(); page = h in pages ? h : 'overview'; const p = pages[page]; $('#view').innerHTML = p.html; $('#hdrTitle').innerHTML = 'chronos / <b>' + p.title.toLowerCase().replace(/ /g, '-') + '</b>';
  document.querySelectorAll('#nav a').forEach(a => a.classList.toggle('on', a.dataset.k === page)); if (p.init) p.init(); if (S && p.update) p.update(S); }
addEventListener('hashchange', route); addEventListener('resize', () => S && pages[page].update && pages[page].update(S));
function toast(m, kind = '') { const t = document.createElement('div'); t.className = 'toast ' + kind; t.textContent = m; $('#toasts').append(t); setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 300); }, 3200); }
// ---- command palette (⌘K / Ctrl+K) ----
const act = async (f, ok) => { try { await f(); toast(ok); } catch (e) { toast(e.message, 'err'); } };
const CMDS = [...nav.map(([k, t], i) => [t, 'go to page ' + (i + 1), () => location.hash = k]), ['Landing page', 'home', () => location.hash = 'home'],
  ['Start live demo (mixed)', 'simulator', () => act(() => api('/api/sim', { action: 'start', workload: 'mixed', rate: 50000 }), 'demo started')], ['Start uniform / dense workload', 'simulator', () => act(() => api('/api/sim', { action: 'start', workload: 'uniform', rate: 100000 }), 'dense workload started')],
  ['Start sparse workload', 'simulator', () => act(() => api('/api/sim', { action: 'start', workload: 'sparse', rate: 100000 }), 'sparse workload started')], ['Stop simulator', 'simulator', () => act(() => api('/api/sim', { action: 'stop' }), 'simulator stopped')],
  ...['adaptive', 'tree', 'array'].map(m => ['Index strategy: ' + m, 'resets book', () => act(() => api('/api/engine', { mode: m }), 'strategy → ' + m)])];
let pi = 0, pl = CMDS;
function pRender() { $('#plist').innerHTML = pl.map((c, i) => `<li class="${i === pi ? 'on' : ''}" data-i="${i}">${esc(c[0])}<small>${esc(c[1])}</small></li>`).join('') || '<li class="dim">no match</li>'; const on = $('#plist .on'); on && on.scrollIntoView({ block: 'nearest' }); }
function pOpen(v) { const p = $('#pal'); p.hidden = !v; if (v) { $('#pin').value = ''; pl = CMDS; pi = 0; pRender(); $('#pin').focus(); } }
$('#pin').oninput = e => { const q = e.target.value.toLowerCase(); pl = CMDS.filter(c => (c[0] + c[1]).toLowerCase().includes(q)); pi = 0; pRender(); };
$('#pin').onkeydown = e => { if (e.key === 'ArrowDown') pi = Math.min(pl.length - 1, pi + 1); else if (e.key === 'ArrowUp') pi = Math.max(0, pi - 1); else if (e.key === 'Enter' && pl[pi]) { pOpen(false); pl[pi][2](); return; } else return; e.preventDefault(); pRender(); };
$('#plist').onclick = e => { const li = e.target.closest('li[data-i]'); if (li) { pOpen(false); pl[+li.dataset.i][2](); } };
$('#pal').onmousedown = e => { if (e.target.id === 'pal') pOpen(false); };
addEventListener('keydown', e => { if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') { e.preventDefault(); pOpen($('#pal').hidden); } else if (e.key === 'Escape') pOpen(false); });
$('#kPal').onclick = () => pOpen(true);
addEventListener('DOMContentLoaded', () => { route(); connect(); });
