'use strict';
// CHRONOS · web2 (Active-Theory-inspired). All numbers come from the engine over WebSocket or results/*.json.
const $ = (s, r = document) => r.querySelector(s), $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fmt = (n, d = 0) => { n = +n; if (!isFinite(n)) return '–'; const a = Math.abs(n); return a >= 1e9 ? (n / 1e9).toFixed(2) + 'B' : a >= 1e6 ? (n / 1e6).toFixed(2) + 'M' : a >= 1e4 ? (n / 1e3).toFixed(1) + 'K' : n.toLocaleString(undefined, { maximumFractionDigits: d }); };
const ns = n => !isFinite(n) ? '–' : n >= 1e6 ? (n / 1e6).toFixed(2) + ' ms' : n >= 1e3 ? (n / 1e3).toFixed(2) + ' µs' : Math.round(n) + ' ns';
const mb = b => (b / 1048576).toFixed(b >= 1e8 ? 0 : 1) + ' MB';
const COL = { std: '#6a6a6a', tree: '#a8a8a8', array: '#ffffff', adaptive: '#9aa0ff' };
const NAME = { std: 'STL baseline', tree: 'Custom · tree', array: 'Custom · array', adaptive: 'CHRONOS-ADAPT' };
const RM = matchMedia('(prefers-reduced-motion: reduce)').matches;  // no continuous motion when the user asked for less
const PARTS_MAX = 320;  // render-budget cap on live particles

async function api(path, body) {
  const r = await fetch(path, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({ error: 'bad response' }));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
function toast(m, err) { const t = document.createElement('div'); t.className = 'toast' + (err ? ' err' : ''); t.textContent = m; $('#toasts').append(t); setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 400); }, 3200); }
const run = async (f, ok) => { try { const r = await f(); toast(typeof ok === 'function' ? ok(r) : ok); return r; } catch (e) { toast(e.message, true); } };

// ====================================================================== scene (full-screen canvas)
const bg = $('#bg'), g = bg.getContext('2d');
let W = 0, H = 0, D = 1;
const V = { parts: [], pulses: [], spawnQ: 0, tradeQ: 0, p: 0, rot: 0, mx: .5, my: .5, view: 'stage', sc: 1, cx: 0, cy: 0, R: 150, a: 1, geo: null };
let stars = [], aurora = null;
function resize() { D = Math.min(devicePixelRatio || 1, 1.5); W = innerWidth; H = innerHeight; bg.width = W * D; bg.height = H * D; g.setTransform(D, 0, 0, D, 0, 0);
  stars = Array.from({ length: Math.min(480, Math.round(W * H / 7000)) }, () => ({ x: Math.random(), y: Math.random(), z: Math.random() * .9 + .1, t: Math.random() * 6.28 }));
  aurora = document.createElement('canvas'); aurora.width = Math.ceil(W / 4); aurora.height = Math.ceil(H / 4); const a = aurora.getContext('2d'), gr = a.createRadialGradient(0, 0, 0, 0, 0, Math.max(aurora.width, aurora.height) * .75); gr.addColorStop(0, 'rgba(70,130,115,.17)'); gr.addColorStop(1, 'rgba(0,0,0,0)'); a.fillStyle = gr; a.fillRect(0, 0, aurora.width, aurora.height); }
function geo() { const m = $('.mid'), b = $('#bk'), a = $('#ak'); V.geo = m && b && a ? { m: m.getBoundingClientRect(), l: b.getBoundingClientRect().right, r: a.getBoundingClientRect().left } : null; }
addEventListener('resize', () => { resize(); V.view = viewMode(location.hash.slice(1), document.body.classList.contains('stage')); geo(); if (S) VIEWS[page] && VIEWS[page].update && VIEWS[page].update(S); });
addEventListener('pointermove', e => { V.mx = e.clientX / innerWidth; V.my = e.clientY / innerHeight; });
resize();

function target() { const m = Math.min(W, H);
  if (V.view === 'stage') return { cx: (W < 760 ? .5 : .66) * W, cy: .44 * H, R: m * (W < 760 ? .26 : .2), a: 1 };
  if (V.view === 'floor' && V.geo) { const r = V.geo.m; return { cx: r.left + r.width / 2, cy: r.top + r.height * .46, R: Math.max(50, Math.min(r.width * .46, r.height * .4)), a: 1 }; }
  return { cx: W / 2, cy: H / 2, R: m * .28, a: .14 };
}
function spawn() { const side = Math.random() < .5 ? 0 : 1, lanes = V.view === 'stage' && V.p > 1.6 && V.p < 2.6;
  const x0 = V.view === 'floor' && V.geo ? (side ? V.geo.r : V.geo.l) : side ? W + 10 : -10, y0 = lanes ? H * (.2 + .6 * Math.floor(Math.random() * 8) / 7) : H * (.12 + Math.random() * .7);
  V.parts.push({ x: x0, y: y0, px: x0, py: y0, side, s: .012 + Math.random() * .014 }); }
function frame(t) {
  requestAnimationFrame(frame); tween();  // reschedule + update numbers first: a failed draw must not freeze the stat tiles
  if (!RM) V.rot += .0016; const T = target(); for (const k of ['cx', 'cy', 'R', 'a']) V[k] += (T[k] - V[k]) * .07;
  const scT = V.view === 'stage' ? (V.p > 1.5 && V.p < 2.5 ? .86 : V.p > 3.5 ? 1.12 : 1) : 1; V.sc += (scT - V.sc) * .05;
  g.globalAlpha = 1; g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
  if (aurora.width && aurora.height) g.drawImage(aurora, 0, 0, W, H);  // 0-size canvas (page loaded in a hidden/zero-size window) throws
  for (const s of stars) { const x = (((s.x * W - (V.mx - .5) * 40 * s.z) % W) + W) % W, y = s.y * H - (V.my - .5) * 24 * s.z; g.globalAlpha = (.25 + .45 * Math.abs(Math.sin(t / 1800 + s.t))) * s.z * (V.view === 'stage' ? 1 : .6); g.fillStyle = '#fff'; g.fillRect(x, y, s.z * 1.6, s.z * 1.6); }
  const cx = V.cx + (V.mx - .5) * 22, cy = V.cy + (V.my - .5) * 14, R = V.R * V.sc, A = V.a;
  g.globalAlpha = A;
  const gl = g.createRadialGradient(cx, cy, R * .6, cx, cy, R * 1.5); gl.addColorStop(0, 'rgba(154,160,255,.0)'); gl.addColorStop(.62, 'rgba(154,160,255,.22)'); gl.addColorStop(1, 'rgba(154,160,255,0)'); g.fillStyle = gl; g.beginPath(); g.arc(cx, cy, R * 1.5, 0, 6.2832); g.fill();
  g.strokeStyle = 'rgba(255,255,255,.75)'; g.lineWidth = 1.3; g.beginPath(); g.arc(cx, cy, R, 0, 6.2832); g.stroke();
  g.strokeStyle = 'rgba(255,255,255,.35)'; g.lineWidth = 1; for (let i = 0; i < 96; i++) { const a = V.rot + i * 6.2832 / 96, r1 = R * .86, r2 = R * (i % 6 ? .91 : .96); g.beginPath(); g.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1); g.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2); g.stroke(); }
  g.strokeStyle = 'rgba(255,255,255,.8)'; g.beginPath(); for (let i = 0; i <= 6; i++) { const a = -V.rot * 1.6 + i * 1.0472, r = R * .34; i ? g.lineTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r) : g.moveTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r); } g.stroke();
  g.strokeStyle = 'rgba(255,255,255,.25)'; for (let i = 0; i < 3; i++) { const a = -V.rot * 1.6 + i * 2.0944; g.beginPath(); g.moveTo(cx + Math.cos(a) * R * .34, cy + Math.sin(a) * R * .34); g.lineTo(cx - Math.cos(a) * R * .34, cy - Math.sin(a) * R * .34); g.stroke(); }
  g.fillStyle = '#fff'; g.fillRect(cx - 2, cy - 2, 4, 4);
  // order particles: spawned from real order-count deltas
  for (let n = 0; n < 3 && V.spawnQ >= 1 && V.parts.length < PARTS_MAX; n++) { spawn(); V.spawnQ--; }
  for (let i = V.parts.length - 1; i >= 0; i--) { const p = V.parts[i], dx = cx - p.x, dy = cy - p.y, d = Math.hypot(dx, dy); p.px = p.x; p.py = p.y; p.x += dx * p.s + Math.sign(dx) * 1.1; p.y += dy * p.s;
    g.strokeStyle = p.side ? 'rgba(154,160,255,.8)' : 'rgba(255,255,255,.8)'; g.lineWidth = 1.2; g.beginPath(); g.moveTo(p.px, p.py); g.lineTo(p.x, p.y); g.stroke();
    if (d < R * .9) { V.parts.splice(i, 1); if (V.tradeQ >= 1) { V.tradeQ--; V.pulses.push({ r: R * .9, a: .9 }); } } }
  for (let i = V.pulses.length - 1; i >= 0; i--) { const u = V.pulses[i]; u.r += R * .018; u.a -= .014; if (u.a <= 0) { V.pulses.splice(i, 1); continue; } g.strokeStyle = `rgba(255,255,255,${u.a * A})`; g.lineWidth = 1.5; g.beginPath(); g.arc(cx, cy, u.r, 0, 6.2832); g.stroke(); }
  g.globalAlpha = 1;
}

// ====================================================================== live state
let S = null, page = 'floor', prevRecv = null, prevTrades = null, META = null, VIEWS = {}, WS = null, numEls = [], cleanup = [], lastNote = '';
const ser = { ops: [], avg: [], p99: [] }; const push = (a, v) => { a.push(v); if (a.length > 160) a.shift(); };
let trades = [], tradeSort = { k: 0, dir: -1 }, tradeF = {};
const tgt = {}, cur = {};
function tween() { for (const el of numEls) { const k = el.dataset.n, t = tgt[k]; if (t === undefined) continue; cur[k] = (cur[k] ?? 0) + (t - (cur[k] ?? 0)) * .15; if (Math.abs(t - cur[k]) < .6) cur[k] = t; el.textContent = el.dataset.f === 'ns' ? ns(cur[k]) : el.dataset.f === 'mb' ? mb(cur[k]) : fmt(Math.round(cur[k])); } }
function connect() {
  const ws = WS = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  ws.onopen = () => { setLive(true); sentDepth = 0; sendDepth(); }; ws.onclose = () => { setLive(false); WS = null; setTimeout(connect, 1500); }; ws.onerror = () => ws.close();
  ws.onmessage = e => { S = JSON.parse(e.data); onSnap(); };
}
let sentDepth = 0, wantDepth = 0;
function sendDepth(n) { if (n) wantDepth = n; if (WS && WS.readyState === 1 && wantDepth && wantDepth !== sentDepth) { WS.send(JSON.stringify({ depth: wantDepth })); sentDepth = wantDepth; } }
function setLive(on) { $('#liveDot').classList.toggle('on', on); if (!on) $('#liveT').textContent = 'engine disconnected'; }
function onSnap() {
  const t = S.stats;
  tgt.recv = t.received; tgt.trades = t.trades; tgt.active = t.activeBuy + t.activeSell; tgt.ops = t.opsPerSec; tgt.p99 = t.p99Ns; tgt.mem = t.heapBytes;
  $('#liveT').textContent = S.sim ? `live · ${fmt(t.opsPerSec)} orders/s · ${S.adapt.usingArray ? 'array' : 'tree'} index` : 'engine idle — start the market on the floor';
  if (prevRecv !== null && S.sim && !RM && t.received >= prevRecv) { V.spawnQ = Math.min(V.spawnQ + Math.min((t.received - prevRecv) / 260, 40), 60); V.tradeQ = Math.min(V.tradeQ + Math.min(Math.max(0, t.trades - prevTrades) / 220, 10), 24); }
  if (S.simNote && S.simNote !== lastNote) toast(S.simNote, true); lastNote = S.simNote || '';
  if (prevRecv !== null && t.received < prevRecv) { trades = []; ser.ops.length = ser.avg.length = ser.p99.length = 0; }
  prevRecv = t.received; prevTrades = t.trades;
  push(ser.ops, t.opsPerSec); push(ser.avg, t.avgNs); push(ser.p99, t.p99Ns);
  for (const x of S.trades || []) trades.push(x); if (trades.length > 5000) trades.splice(0, trades.length - 5000);
  if (document.body.classList.contains('stage')) return;
  const v = VIEWS[page]; v && v.update && v.update(S);
}

// ====================================================================== tiny charts
function prep(cv) { const w = cv.clientWidth, h = cv.clientHeight; if (cv._w !== w || cv._ht !== h || cv._dpr !== D) { cv.width = w * D; cv.height = h * D; cv._w = w; cv._ht = h; cv._dpr = D; } const c = cv.getContext('2d'); c.setTransform(D, 0, 0, D, 0, 0); c.clearRect(0, 0, w, h); c.font = '11px ui-monospace, Menlo, monospace'; return { c, w, h }; }
function grid(c, w, h, max, L, B, f) { c.strokeStyle = '#222'; c.fillStyle = '#808080'; c.lineWidth = 1; for (let i = 0; i <= 3; i++) { const y = 6 + (h - B - 6) * i / 3; c.beginPath(); c.moveTo(L, y); c.lineTo(w, y); c.stroke(); c.fillText(f(max * (1 - i / 3)), 0, y + 3); } }
function line(cv, sets, f = fmt) { const { c, w, h } = prep(cv), L = 46, B = 4, n = Math.max(2, ...sets.map(s => s.d.length)), mx = Math.max(1e-9, ...sets.flatMap(s => s.d)) * 1.1; grid(c, w, h, mx, L, B, f);
  sets.forEach(s => { c.strokeStyle = s.c; c.lineWidth = 1.5; c.beginPath(); s.d.forEach((v, i) => { const x = L + (w - L) * i / (n - 1), y = 6 + (h - B - 6) * (1 - v / mx); i ? c.lineTo(x, y) : c.moveTo(x, y); }); c.stroke(); }); }
function bars(cv, groups, keys, f) { const draw = () => { const { c, w, h } = prep(cv), L = 50, B = 18, mx = Math.max(1e-9, ...groups.flatMap(x => keys.map(k => x.v[k] || 0))) * 1.1; grid(c, w, h, mx, L, B, f);
    const gw = (w - L) / groups.length, bw = Math.min(24, gw * .8 / keys.length); let hit = null;
    groups.forEach((x, i) => { const x0 = L + gw * i + (gw - bw * keys.length) / 2; keys.forEach((k, j) => { const v = x.v[k] || 0, bh = (h - B - 6) * v / mx, bx = x0 + bw * j, by = h - B - bh, on = cv._x != null && cv._x >= bx && cv._x < bx + bw && cv._y > by - 4 && cv._y < h - B; c.globalAlpha = cv._x != null && !on ? .5 : 1; c.fillStyle = COL[k]; c.fillRect(bx, by, bw - 2, bh); c.globalAlpha = 1; if (on) hit = { k, v, l: x.l, bx, by }; });
      c.fillStyle = '#999'; c.textAlign = 'center'; c.fillText(x.l, L + gw * i + gw / 2, h - 4); c.textAlign = 'left'; });
    if (hit) { const t = `${NAME[hit.k]} · ${f(hit.v)}`, tw = c.measureText(t).width + 14, tx = Math.min(w - tw, Math.max(0, hit.bx - tw / 2)), ty = Math.max(0, hit.by - 26); c.fillStyle = '#fff'; c.fillRect(tx, ty, tw, 20); c.fillStyle = '#000'; c.fillText(t, tx + 7, ty + 14); } };
  cv._draw = draw; if (!cv._hv) { cv._hv = 1; cv.onmousemove = e => { const r = cv.getBoundingClientRect(); cv._x = e.clientX - r.left; cv._y = e.clientY - r.top; cv._draw(); }; cv.onmouseleave = () => { cv._x = null; cv._draw(); }; } draw(); }

// ====================================================================== views
const rowsHtml = cls => `<div class="rows ${cls}"></div>`;
const ROW_PX = () => parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--row')) || 26;
const rowEl = '<div class="rw"><span class="p">–</span><span>–</span><span>–</span></div>';
const seg = (id, opts, cur) => `<div class="seg" id="${id}">${opts.map(([v, t]) => `<button data-v="${v}" class="${v === cur ? 'on' : ''}">${t}</button>`).join('')}</div>`;
const prevQ = {};
let polling = false;
function fillBook(sel, rows, side) { const els = $$(sel + ' .rw'), mx = rows.length ? rows[rows.length - 1][3] : 1;
  els.forEach((el, i) => { const r = rows[i]; if (!r) { el.style.visibility = 'hidden'; return; } el.style.visibility = ''; el.classList.toggle('best', i === 0); el.children[0].textContent = r[0]; el.children[1].textContent = fmt(r[1]); el.children[2].textContent = r[2]; el.style.setProperty('--w', (r[3] / mx).toFixed(3));
    const k = side + r[0], o = prevQ[k]; if (o !== undefined && o !== r[1]) { el.classList.remove('up'); void el.offsetWidth; el.classList.add('up'); } prevQ[k] = r[1]; }); }

function makeViews() { const B = META.band, MID = Math.floor((B[0] + B[1]) / 2), LIM = META.limits, DEF = META.defaults, WL = META.workloads; return {
  floor: { html: `<div class="pg floor">
      <section class="pn"><h3>Buyers <small>highest price first</small></h3><div class="rh"><span>Price</span><span>Size</span><span>Orders</span></div><div id="bk">${rowsHtml('bids')}</div></section>
      <section class="mid"><div class="mp"><small>MID PRICE · TICKS</small><b id="mid">–</b><span id="spr">waiting for orders…</span></div>
        <div class="chips"><div class="chip"><small>orders / second</small><b data-n="ops">–</b></div><div class="chip"><small>p99 latency</small><b data-n="p99" data-f="ns">–</b></div><div class="chip"><small>resting orders</small><b data-n="active">–</b></div><div class="chip"><small>trades</small><b data-n="trades">–</b></div></div>
        <div class="ctl"><button class="pill" id="go">▶ Start live market</button><button class="gh" id="stop">Stop</button>${seg('sm', [['adaptive', 'Adaptive'], ['tree', 'Tree'], ['array', 'Array']], 'adaptive')}</div></section>
      <section class="pn"><h3>Sellers <small>lowest price first</small></h3><div class="rh"><span>Price</span><span>Size</span><span>Orders</span></div><div id="ak">${rowsHtml('asks')}</div></section>
      <section class="pn tp"><div><h3>Trade tape</h3><div class="tape" id="tape"></div></div><div><h3>Throughput <small>orders/s</small></h3><div class="cw"><canvas id="c1"></canvas></div></div><div><h3>Latency <small>avg · p99</small></h3><div class="cw"><canvas id="c2"></canvas></div></div></section></div>`,
    init() { const fit = () => { const n = Math.max(META.limits.minDepth, Math.min(META.limits.maxDepth, Math.floor($('#bk').clientHeight / ROW_PX())));
        for (const sel of ['#bk .rows', '#ak .rows']) if ($$(sel + ' .rw').length !== n) $(sel).innerHTML = rowEl.repeat(n); this.tapeN = Math.max(3, Math.floor($('#tape').clientHeight / ROW_PX())); sendDepth(n); geo(); if (S) this.update(S); };
      const ro = new ResizeObserver(fit); ro.observe($('#bk')); ro.observe($('#tape')); cleanup.push(() => ro.disconnect()); fit();
      $('#go').onclick = () => run(() => api('/api/sim', { action: 'start', workload: DEF.workload, rate: DEF.rate }), 'market started');
      $('#stop').onclick = () => run(() => api('/api/sim', { action: 'stop' }), 'market stopped');
      $('#sm').onclick = e => { const b = e.target.closest('button'); if (b) run(() => api('/api/engine', { mode: b.dataset.v }), 'strategy → ' + b.dataset.v + ' (book reset)'); }; },
    update(s) { fillBook('#bk', s.book.bids, 'b'); fillBook('#ak', s.book.asks, 'a'); const b = s.book.bids[0], a = s.book.asks[0];
      $('#mid').textContent = a && b ? ((a[0] + b[0]) / 2).toFixed(1) : '–'; $('#spr').textContent = a && b ? `best bid ${b[0]} · best ask ${a[0]} · spread ${a[0] - b[0]}` : 'waiting for orders…';
      $('#go').disabled = s.sim; $('#go').textContent = s.sim ? '● Market running' : '▶ Start live market'; $$('#sm button').forEach(x => x.classList.toggle('on', x.dataset.v === s.mode));
      $('#tape').innerHTML = trades.slice(-this.tapeN).reverse().map(t => `<div><span>#${t[0]}</span><span class="q">${fmt(t[4])} units</span><span>@ ${t[3]}</span></div>`).join('') || '<div class="q">no trades yet</div>';
      line($('#c1'), [{ d: ser.ops, c: '#fff' }]); line($('#c2'), [{ d: ser.avg, c: '#fff' }, { d: ser.p99, c: '#9aa0ff' }], ns); } },

  orders: { html: `<div class="pg orders"><div class="col">
      <section class="pn"><h3>Place an order</h3><form class="f" id="fo"><div class="two"><label>Side<select name="side"><option value="buy">Buy</option><option value="sell">Sell</option></select></label><label>Type<select name="type"><option value="limit">Limit (set a price)</option><option value="market">Market (any price)</option></select></label></div>
        <div class="two"><label>Price (${B[0]}–${B[1] - 1})<input name="price" type="number" step="1" min="${B[0]}" max="${B[1] - 1}" value="${MID}"></label><label>Quantity<input name="qty" type="number" step="1" min="1" max="${LIM.maxQty}" value="100" required></label></div><button class="pill" type="submit">Submit order</button></form></section>
      <section class="pn"><h3>Cancel or change</h3><form class="f" id="fc"><div class="two"><label>Order id<input name="id" type="number" min="1" step="1" required></label><span></span></div><div class="three"><label>New price<input name="price" type="number" min="${B[0]}" max="${B[1] - 1}" step="1"></label><label>New quantity<input name="qty" type="number" min="1" step="1"></label><span></span></div><div class="row2" style="display:flex;gap:10px"><button class="gh" type="submit" value="cancel">Cancel order</button><button class="gh" type="submit" value="modify">Change (loses queue place)</button></div></form></section>
      <section class="pn"><h3>Deterministic highly optimised feed <small>seeded · synthetic orders</small></h3><form class="f" id="fs"><div class="two"><label>Pattern<select name="workload">${WL.map(w => `<option ${w === DEF.workload ? 'selected' : ''}>${w}</option>`).join('')}</select></label><label>Orders / second<input name="rate" type="number" min="1" max="${LIM.maxRate}" value="${DEF.rate}"></label></div>
        <div class="two"><label>Index strategy<select name="mode">${META.modes.map(m => `<option value="${m}" ${m === DEF.mode ? 'selected' : ''}>${m === 'adaptive' ? 'CHRONOS-ADAPT' : 'Fixed ' + m}</option>`).join('')}</select></label><label>Cancel ratio (blank = preset)<input name="cancel" type="number" step="0.05" min="0" max="${LIM.maxCancel}"></label></div>
        <div style="display:flex;gap:10px"><button class="pill" type="submit">Start (resets book)</button><button class="gh" type="button" id="sx">Stop</button></div></form></section></div>
      <section class="pn"><h3>Engine responses <small>every order shows what the engine did</small></h3><div class="log" id="log"><div>Submit an order on the left. A buy at or above the best ask trades immediately; otherwise it rests in the book.</div></div></section></div>`,
    init() { const log = (m, c) => $('#log').insertAdjacentHTML('afterbegin', `<div class="${c || ''}">${esc(m)}</div>`), num = (f, k) => { const v = f.get(k); return v === '' || v === null ? undefined : Number(v); };
      const go = async (path, b) => { try { const r = await api(path, b); log(path.slice(5) + ' → ' + (r.status ? r.status + (r.trades && r.trades.length ? `, ${r.trades.length} trade(s): ` + r.trades.map(t => `${t.qty}@${t.price}`).join(' ') : '') + (r.id ? `  [id ${r.id}]` : '') : 'ok'), /^rej/.test(r.status || '') ? 'e' : 'o'); toast(r.status || 'ok', /^rej/.test(r.status || '')); } catch (e) { log(path.slice(5) + ' ✗ ' + e.message, 'e'); toast(e.message, true); } };
      const ts = $('#fo [name=type]'), pr = $('#fo [name=price]'); ts.onchange = () => { pr.disabled = ts.value === 'market'; }; ts.onchange();
      $('#fo').onsubmit = e => { e.preventDefault(); const f = new FormData(e.target), b = { side: f.get('side'), type: f.get('type'), qty: num(f, 'qty') }; if (b.type === 'limit') b.price = num(f, 'price'); go('/api/order', b); };
      $('#fc').onsubmit = e => { e.preventDefault(); const f = new FormData(e.target), act = e.submitter && e.submitter.value, id = num(f, 'id'); if (act === 'cancel') return go('/api/cancel', { id }); const p = num(f, 'price'), q = num(f, 'qty'); if (p === undefined || q === undefined) return log('to change an order, fill in both new price and new quantity', 'e'); go('/api/modify', { id, price: p, qty: q }); };
      $('#fs').onsubmit = async e => { e.preventDefault(); const f = new FormData(e.target); await go('/api/engine', { mode: f.get('mode') }); const b = { action: 'start', workload: f.get('workload'), rate: num(f, 'rate') }; const c = num(f, 'cancel'); if (c !== undefined) b.cancel = c; go('/api/sim', b); };
      $('#sx').onclick = () => go('/api/sim', { action: 'stop' }); }, update() {} },

  trades: { html: `<div class="pg trades"><section class="pn"><h3>Trade history <small id="tn"></small></h3><div class="three" style="grid-template-columns:repeat(4,1fr);margin-bottom:12px"><label>Min quantity<input id="fq" type="number" min="0"></label><label>Min price<input id="fl" type="number"></label><label>Max price<input id="fh" type="number"></label><label>Order id (buyer or seller)<input id="fi" type="number" min="1"></label></div>
      <div class="fill"><table class="tbl"><thead><tr id="th"></tr></thead><tbody id="tb"></tbody></table></div></section></div>`,
    init() { $('#th').innerHTML = ['Trade', 'Buyer order', 'Seller order', 'Price', 'Quantity', 'Time'].map((c, i) => `<th class="s" data-k="${i}">${c}</th>`).join('');
      $('#th').onclick = e => { const k = e.target.dataset.k; if (k === undefined) return; tradeSort = { k: +k, dir: tradeSort.k === +k ? -tradeSort.dir : -1 }; this.update(); };
      ['fq', 'fl', 'fh', 'fi'].forEach(i => $('#' + i).oninput = () => { tradeF[i] = $('#' + i).value; this.update(); });
      api('/api/trades?limit=2000').then(a => { const seen = new Set(trades.map(t => t[0])); trades = a.filter(t => !seen.has(t[0])).concat(trades); this.update(); }).catch(() => {}); },
    update() { const n = k => tradeF[k] === undefined || tradeF[k] === '' ? null : +tradeF[k];
      const r = trades.filter(t => (n('fq') === null || t[4] >= n('fq')) && (n('fl') === null || t[3] >= n('fl')) && (n('fh') === null || t[3] <= n('fh')) && (n('fi') === null || t[1] === n('fi') || t[2] === n('fi'))).sort((a, b) => (a[tradeSort.k] - b[tradeSort.k]) * tradeSort.dir);
      $('#tb').innerHTML = r.slice(0, 400).map(t => `<tr><td>${t[0]}</td><td>${t[1]}</td><td>${t[2]}</td><td>${t[3]}</td><td>${fmt(t[4])}</td><td>${new Date(t[5]).toLocaleTimeString()}.${String(t[5] % 1000).padStart(3, '0')}</td></tr>`).join('') || '<tr><td colspan="6" class="l" style="color:#808080">no trades match. Start the market on the Floor page.</td></tr>';
      $('#tn').textContent = `${r.length} matching · latest first by default`; } },

  adapt: { html: `<div class="pg adapt"><section class="pn"><h3>CHRONOS-ADAPT <small>live decision</small></h3><span class="badge" id="ab">–</span><p class="why2" id="ar">–</p><div class="mets" id="mets"></div><p class="note" id="ac"></p></section>
      <section class="pn"><h3>Switch history <small>each migration is validated before it replaces the old index</small></h3><div class="fill"><table class="tbl"><thead><tr><th>At command</th><th>Direction</th><th>Took</th><th>Orders</th><th>Levels</th><th>Memory before</th><th>Memory after</th><th class="l">Why</th></tr></thead><tbody id="ae"></tbody></table></div></section></div>`,
    init() { $('#mets').innerHTML = ['Price levels', 'Level density', 'Inserts / op', 'Cancels / op', 'Level churn / op', 'Best-price lookups / op', 'Recent ns / op', 'Index + book memory'].map((t, i) => `<div class="met"><small>${t}</small><b id="m${i}">–</b></div>`).join(''); },
    update(s) { const a = s.adapt, c = a.cfg; const b = $('#ab'); b.textContent = a.usingArray ? 'ARRAY + BITMAP' : 'TREE'; b.className = 'badge' + (a.usingArray ? ' arr' : '');
      $('#ar').textContent = s.mode === 'adaptive' ? a.reason : `Fixed strategy (${s.mode}): adaptation is switched off so you can compare.`;
      [fmt(a.levels), (+a.density).toFixed(4), (+a.insertRate).toFixed(3), (+a.cancelRate).toFixed(3), (+a.churn).toFixed(3), (+a.bestRate).toFixed(2), ns(a.nsPerOp), mb(a.bytes)].forEach((v, i) => $('#m' + i).textContent = v);
      $('#ac').textContent = c.policy === 1 ? `Policy v1: switch to array when density ≥ ${c.denseHi} (checked every ${fmt(c.batch)} commands).` : `Policy v2: switch to array when levels ≥ ${fmt(c.levelsHi)}, churn ≥ ${c.minChurn} and the array fits ${c.memBudgetMB} MB; back to tree below ${c.levelsLo} levels. Checked every ${fmt(c.batch)} commands, ${c.confirm} agreeing checks required.`;
      $('#ae').innerHTML = a.events.slice().reverse().map(e => `<tr><td>${fmt(e.op)}</td><td class="l">${e.toArray ? 'tree → array' : 'array → tree'}</td><td>${e.ms.toFixed(2)} ms</td><td>${fmt(e.orders)}</td><td>${fmt(e.levels)}</td><td>${mb(e.before)}</td><td>${mb(e.after)}</td><td class="l" style="color:#999">${esc(e.why)}</td></tr>`).join('') || '<tr><td colspan="8" class="l" style="color:#808080">No switches yet. On the Orders page run the “uniform” pattern in adaptive mode to see one.</td></tr>'; } },

  lab: { html: `<div class="pg lab"><section class="pn"><h3>Benchmark lab <small>separate process</small></h3><form class="f" id="bf"><div class="two"><label>Workload<select name="workload"><option value="all">all six</option>${WL.map(w => `<option>${w}</option>`).join('')}</select></label><label>Orders<input name="n" type="number" min="${LIM.minBenchN}" max="${LIM.maxBenchN}" step="1000" value="200000"></label></div>
        <div class="two"><label>Trials<input name="trials" type="number" min="1" max="${LIM.maxTrials}" value="3"></label><label>Cancel % (blank = preset)<input name="cancel" type="number" min="0" max="${LIM.maxCancel * 100}" step="5"></label></div>
        <div class="chk">${META.strategies.map(k => `<label><input type="checkbox" name="s_${k}" checked><i style="width:9px;height:9px;border-radius:50%;background:${COL[k]}"></i>${NAME[k]}</label>`).join('')}</div>
        <button class="pill" type="submit" id="bgo">Run benchmark</button><div class="pr"><div id="bp"></div></div><div class="note" id="bs" style="margin:0">The live market pauses during a run so it cannot skew timings.</div></form><hr>
        <div class="f"><label>Result file<select id="bfile"></select></label><div class="two"><label>Compare with<select id="bcmp"><option value="">(none)</option></select></label><label>Metric<select id="bmet"><option value="ops">Throughput</option><option value="p99">p99 latency</option><option value="p999">p99.9 latency</option><option value="mem">Heap memory</option></select></label></div>
        <div style="display:flex;gap:10px"><button class="gh" type="button" id="bj">Export JSON</button><button class="gh" type="button" id="bc2">Export CSV</button></div><div class="note" id="bcfg" style="margin:0"></div></div></section>
      <div class="right"><section class="pn"><h3>Comparison <small>medians over trials</small></h3><div class="leg" id="bl"></div><div class="cw"><canvas id="bcv"></canvas></div></section>
      <section class="pn"><h3>Numbers <small>✓ = identical trades on every strategy</small></h3><div class="fill"><table class="tbl"><thead><tr><th>Workload</th><th>Strategy</th><th>Mops/s</th><th>avg</th><th>p99</th><th>p99.9</th><th>max</th><th>Heap MB</th><th>Allocs</th><th>Switches</th><th>vs</th><th>✓</th></tr></thead><tbody id="bt"></tbody></table></div></section></div></div>`,
    lab: {},
    init() { const L = this.lab; const pick = async () => { const l = (await api('/api/bench/list').catch(() => [])).sort(); $('#bfile').innerHTML = l.map(f => `<option>${esc(f)}</option>`).join('') || '<option value="">none yet. Run ./run_benchmarks.sh</option>'; $('#bcmp').innerHTML = '<option value="">(none)</option>' + l.map(f => `<option>${esc(f)}</option>`).join(''); draw(); };
      const draw = async () => { try { L.a = $('#bfile').value ? await api('/api/bench/results?file=' + $('#bfile').value) : null; L.b = $('#bcmp').value ? await api('/api/bench/results?file=' + $('#bcmp').value) : null; } catch (e) { return toast(e.message, true); } if (!L.a) return;
        const m = $('#bmet').value, ss = META.strategies.filter(k => L.a.results.some(r => r.strategy === k)), ws = [...new Set(L.a.results.map(r => r.workload))], val = r => m === 'ops' ? r.opsPerSec / 1e6 : m === 'p99' ? r.lat.p99 : m === 'p999' ? r.lat.p999 : r.heapMB, f = m === 'ops' ? v => v.toFixed(1) : m === 'mem' ? v => v.toFixed(0) : ns;
        $('#bl').innerHTML = ss.map(k => `<span><i style="background:${COL[k]}"></i>${NAME[k]}</span>`).join(''); bars($('#bcv'), ws.map(w => ({ l: w.replace('_', ' '), v: Object.fromEntries(L.a.results.filter(r => r.workload === w).map(r => [r.strategy, val(r)])) })), ss, f);
        const c = L.a.config; $('#bcfg').textContent = `n=${fmt(c.n)} · seed ${c.seed} · ${c.trials} trials · band ${c.band[1] - c.band[0]} ticks · policy v${c.policy ?? '?'}`;
        const base = {}; L.a.results.forEach(r => { if (r.strategy === 'tree') base[r.workload] = r.opsPerSec; });
        $('#bt').innerHTML = L.a.results.map(r => { const o = L.b && L.b.results.find(x => x.workload === r.workload && x.strategy === r.strategy); const vs = o ? (r.opsPerSec / o.opsPerSec).toFixed(2) + '× other file' : base[r.workload] ? (r.opsPerSec / base[r.workload]).toFixed(2) + '× tree' : '';
          return `<tr><td class="l">${esc(r.workload)}</td><td class="l" style="color:${COL[r.strategy]}">${NAME[r.strategy]}</td><td>${(r.opsPerSec / 1e6).toFixed(2)}</td><td>${ns(r.lat.avg)}</td><td>${ns(r.lat.p99)}</td><td>${ns(r.lat.p999)}</td><td>${ns(r.lat.max)}</td><td>${r.heapMB.toFixed(1)}</td><td>${fmt(r.allocs)}</td><td>${r.migrations}</td><td>${vs}</td><td>${r.consistent ? '✓' : '✗'}</td></tr>`; }).join(''); };
      ['bfile', 'bcmp', 'bmet'].forEach(i => $('#' + i).onchange = draw);
      const dl = (n, t, ty) => { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([t], { type: ty })); a.download = n; a.click(); URL.revokeObjectURL(a.href); };
      $('#bj').onclick = () => L.a && dl('chronos_bench.json', JSON.stringify(L.a, null, 1), 'application/json');
      $('#bc2').onclick = () => L.a && dl('chronos_bench.csv', 'workload,strategy,ops_per_sec,avg_ns,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,heap_mb,allocs,migrations\n' + L.a.results.map(r => [r.workload, r.strategy, r.opsPerSec, r.lat.avg, r.lat.p50, r.lat.p95, r.lat.p99, r.lat.p999, r.lat.max, r.heapMB, r.allocs, r.migrations].join(',')).join('\n'), 'text/csv');
      const poll = async () => { if (polling) return; polling = true; try { for (;;) { const s = await api('/api/bench/status').catch(() => null); if (!s || !$('#bp')) return; $('#bp').style.transform = `scaleX(${s.total ? s.done / s.total : 0})`;
          if (s.running) { $('#bs').textContent = `running ${s.done}/${s.total} · ${s.label}`; await new Promise(r => setTimeout(r, 700)); continue; } if (s.error) $('#bs').textContent = s.error; else if (s.file) { $('#bs').textContent = 'finished → ' + s.file; await pick(); $('#bfile').value = s.file; draw(); } return; } } finally { polling = false; } };
      $('#bf').onsubmit = async e => { e.preventDefault(); const f = new FormData(e.target), b = { workload: f.get('workload'), n: +f.get('n'), trials: +f.get('trials'), strategies: META.strategies.filter(k => f.get('s_' + k)).join(',') }; if (f.get('cancel') !== '') b.cancel = +f.get('cancel') / 100;
        if (!b.strategies) return toast('select at least one strategy', true); try { await api('/api/bench/run', b); toast('benchmark started'); poll(); } catch (er) { toast(er.message, true); } };
      pick(); poll(); }, update() {} },
}; }

// ====================================================================== router + scroll story
const NAV = [['floor', 'Floor'], ['orders', 'Orders'], ['trades', 'Trades'], ['adapt', 'Adapt'], ['lab', 'Lab']];
$('#nav').innerHTML = NAV.map(([k, t]) => `<a href="#${k}" data-k="${k}">${t}</a>`).join('<i></i>');
// the portal gets its own column only where the floor is a 3-column layout; on narrow screens it is a dim backdrop
function viewMode(h, stage) { return stage ? 'stage' : (h === 'floor' || !(h in VIEWS)) && innerWidth > 1000 ? 'floor' : 'dim'; }
function route() {
  const h = location.hash.slice(1), stage = !h || h === 'story';
  document.body.classList.toggle('stage', stage); $('#view').hidden = stage;
  V.view = viewMode(h, stage);
  $$('#nav a').forEach(a => a.classList.toggle('on', a.dataset.k === h));
  if (stage) { cleanup.forEach(f => f()); cleanup = []; $('#view').innerHTML = ''; V.geo = null; numEls = $$('[data-n]'); scenes(); return; }
  cleanup.forEach(f => f()); cleanup = []; page = h in VIEWS ? h : 'floor'; const v = VIEWS[page]; $('#view').innerHTML = v.html; v.init && v.init.call(v); numEls = $$('[data-n]'); geo(); S && v.update && v.update(S);
}
function scenes() {
  if (!document.body.classList.contains('stage')) return; const max = document.documentElement.scrollHeight - innerHeight, p = max > 0 ? scrollY / max * 4 : 0; V.p = p;
  $$('.sc').forEach(el => { const i = +el.dataset.i, d = Math.abs(p - i), o = Math.max(0, Math.min(1, 1.3 - d * 1.7)); el.style.opacity = o.toFixed(3); el.style.transform = `translateY(${((p - i) * -34).toFixed(1)}px)`; el.classList.toggle('vis', o > .5); });
  $('#hint').style.opacity = p < .25 ? 1 : 0;
}
addEventListener('scroll', scenes, { passive: true }); addEventListener('hashchange', route);
addEventListener('keydown', e => { if (e.metaKey || e.ctrlKey || e.altKey || /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName)) return; const i = +e.key - 1; if (i >= 0 && i < NAV.length) location.hash = NAV[i][0]; });
$('#howBtn').onclick = e => { e.preventDefault(); scrollTo({ top: (document.documentElement.scrollHeight - innerHeight) / 4, behavior: 'smooth' }); };
(async function boot() {
  for (;;) { try { META = await api('/api/meta'); break; } catch (e) { $('#liveT').textContent = 'engine unreachable, retrying…'; await new Promise(r => setTimeout(r, 1500)); } }
  VIEWS = makeViews(); route(); Object.assign(V, target()); connect(); requestAnimationFrame(frame);
})();
