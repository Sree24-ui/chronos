'use strict';
// Landing page: every number comes from the live engine (WebSocket snapshots) or results/*.json.
(() => {
  const $ = (s, r = document) => r.querySelector(s), $$ = (s, r = document) => [...r.querySelectorAll(s)];
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
  let built = false, started = false, raf = 0, io = null, res = {}, band = 'd', met = 'ops';
  const tgt = {}, cur = {}, prev = {}, tapeQ = [];

  const STEPS = [
    ['01 · ARRIVE', 'Is this order new?', 'The order lands as a command. An open-addressing hash table (IdMap) answers "have I seen this id?" in O(1), so duplicates are rejected and cancels can find their order without searching.', 'IdMap: id → Node*   (load ≤ 0.5, backward-shift delete)', 'IdMap', 'hash · O(1)'],
    ['02 · LOCATE', 'Jump to the best opposite price.', 'The price index returns the best resting level. Tree mode keeps a cached pointer into a red-black tree; array mode asks a 3-level bitmap, which is three count-zeros instructions.', 'tree: std::map + cached best   |   array: bitmap → ctz/clz', 'Price index', 'tree or array+bitmap'],
    ['03 · MATCH', 'Walk the FIFO queue.', 'Each price level is an intrusive doubly linked list in arrival order. The earliest order fills first, at the resting order’s price, until the incoming order is done or the level is empty.', 'fill = min(incoming, head.rem) → Trade{buy, sell, price, qty}', 'Level queue', 'linked list · O(1) unlink'],
    ['04 · REST', 'Keep the remainder without malloc.', 'Leftover quantity becomes a node from a slab pool and is linked at the tail of its level. Finished nodes go back to a free list, so steady-state trading allocates nothing.', 'Pool<Node>::alloc() / free()   — nodes are 64 B, one cache line', 'Memory pool', 'slab · free list'],
    ['05 · ADAPT', 'Rewire the index while running.', 'Between batches, CHRONOS-ADAPT can switch tree ↔ array. It builds the new index around the same order lists, validates every node, then swaps. Orders are never copied, so priority cannot change.', 'build → validate (price order, FIFO seq, totals) → swap | rollback on failure', 'CHRONOS-ADAPT', 'batch-boundary migration'],
  ];

  function build() {
    $('#dg').innerHTML = '<i class="pk" id="pk"></i>' + STEPS.map((s, i) => `<div class="nd" data-i="${i}"><div class="nt">${s[4]}</div><p>${s[5]}</p></div>`).join('');
    $('#steps').innerHTML = STEPS.map((s, i) => `<div class="st" data-i="${i}"><div class="k">${s[0]}</div><h3>${s[1]}</h3><p>${s[2]}</p><pre>${s[3]}</pre></div>`).join('');
    $('#hbr').innerHTML = [...Array(6)].map((_, i) => `<div class="hr a" id="ha${5 - i}"><b class="sell">–</b><span>–</span><span>–</span></div>`).join('') + '<div class="hsp" id="hsp">–</div>' +
      [...Array(6)].map((_, i) => `<div class="hr b" id="hb${i}"><b class="buy">–</b><span>–</span><span>–</span></div>`).join('');
    $('#tape').innerHTML = [...Array(2)].flatMap(() => [...Array(14)].map(() => '<span>awaiting trades…</span>')).join('');
    io = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); } }), { threshold: 0.12 });
    $$('.rv').forEach(el => io.observe(el));
    const act = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) setStep(+e.target.dataset.i); }), { rootMargin: '-45% 0px -45% 0px' });
    $$('.st').forEach(el => act.observe(el));
    $$('.nd').forEach(el => el.onclick = () => $(`.st[data-i="${el.dataset.i}"]`).scrollIntoView({ block: 'center' }));
    const seg = (id, set) => $(id).onclick = e => { const b = e.target.closest('button'); if (!b) return; $$('button', $(id)).forEach(x => x.classList.toggle('on', x === b)); set(b.dataset.v); drawRes(); };
    seg('#segBand', v => band = v); seg('#segMet', v => met = v);
    Promise.all(['bench_default.json', 'bench_wide.json'].map(f => api('/api/bench/results?file=' + f).catch(() => null))).then(([d, w]) => { res = { d, w }; drawRes(); });
    setStep(0); built = true;
  }

  function setStep(i) {
    $$('.st').forEach(e => e.classList.toggle('on', +e.dataset.i === i));
    $$('.nd').forEach(e => e.classList.toggle('on', +e.dataset.i === i));
    const n = $(`.nd[data-i="${i}"]`), pk = $('#pk'); if (n && pk) pk.style.transform = `translateY(${n.offsetTop + n.offsetHeight / 2 - 4}px)`;
  }

  function drawRes() {
    const d = res[band], bars = $('#bars');
    if (!d) { bars.innerHTML = '<div class="dim" style="padding:40px">No benchmark file found. Run <code>./run_benchmarks.sh</code>.</div>'; return; }
    const ss = ['std', 'tree', 'array', 'adaptive'].filter(k => d.results.some(r => r.strategy === k)), ws = [...new Set(d.results.map(r => r.workload))];
    const val = r => met === 'ops' ? r.opsPerSec / 1e6 : r.heapMB, unit = met === 'ops' ? ' Mops/s' : ' MB heap', get = (w, s) => d.results.find(r => r.workload === w && r.strategy === s);
    const max = Math.max(...d.results.map(val));
    $('#resLeg').innerHTML = ss.map(k => `<span><i style="background:${COLORS[k]}"></i>${NAMES[k]}</span>`).join('');
    bars.classList.remove('in');
    bars.innerHTML = ws.map(w => `<div class="bg"><div class="bs">${ss.map((s, i) => { const r = get(w, s); return r ? `<i class="br" style="--h:${(100 * val(r) / max).toFixed(1)};--c:${COLORS[s]};--i:${i}" data-t="${NAMES[s]} · ${val(r).toFixed(1)}${unit}"></i>` : ''; }).join('')}</div></div>`).join('') +
      '';
    let lab = $('#blabels'); if (!lab) { lab = document.createElement('div'); lab.id = 'blabels'; lab.className = 'labels'; bars.after(lab); }
    lab.innerHTML = ws.map(w => `<span>${w.replace('_', ' ')}</span>`).join('');
    requestAnimationFrame(() => requestAnimationFrame(() => bars.classList.add('in')));
    // insight is computed from the loaded file, never typed in by hand
    const ratio = (a, b) => ws.map(w => get(w, a) && get(w, b) ? val(get(w, a)) / val(get(w, b)) : NaN).filter(isFinite);
    const rg = a => { const r = a; return r.length ? `${Math.min(...r).toFixed(2)}×–${Math.max(...r).toFixed(2)}×` : 'n/a'; };
    $('#insight').innerHTML = (met === 'ops'
      ? `Adaptive runs at ${rg(ratio('adaptive', 'array'))} of fixed-array throughput and ${rg(ratio('adaptive', 'tree'))} of fixed-tree across these workloads. Array is the fastest fixed strategy; adaptive’s job is to avoid paying for it when the book is small.`
      : `Adaptive uses ${rg(ratio('adaptive', 'array'))} of the fixed-array heap and ${rg(ratio('adaptive', 'tree'))} of the fixed-tree heap. The array index has a fixed footprint set by the price band; the tree grows with the number of levels.`) +
      ` <span class="dim">Median of ${d.config.trials} trials · n=${fmt(d.config.n)} · ${d.config.band[1] - d.config.band[0]} ticks · policy v${d.config.policy ?? '?'}.</span>`;
  }

  // ---------- live bindings ----------
  function flash(el, cls) { el.classList.remove('fu', 'fd'); void el.offsetWidth; el.classList.add(cls); }
  function snap(s) {
    const t = s.stats;
    tgt.ops = t.opsPerSec; tgt.p99 = t.p99Ns; tgt.recv = t.received; tgt.trades = t.trades; tgt.active = t.activeBuy + t.activeSell; tgt.levels = t.levels;
    $('#hmode').textContent = s.adapt.usingArray ? 'array+bitmap' : 'tree'; $('#hbs').textContent = s.sim ? `● live · ${s.simKind}` : 'idle';
    const rows = (arr, pre, cls) => arr.slice(0, 6).forEach((r, i) => { const el = $(`#${pre}${i}`); if (!el) return; const [p, q, n, c] = r; const key = pre + i, old = prev[key];
      el.children[0].textContent = p; el.children[1].textContent = fmt(q); el.children[2].textContent = fmt(c); el.style.setProperty('--w', Math.min(1, c / (arr[Math.min(5, arr.length - 1)][3] || 1)).toFixed(3));
      if (old !== undefined && old !== q) flash(el, q > old ? 'fu' : 'fd'); prev[key] = q; });
    rows(s.book.asks, 'ha', 'a'); rows(s.book.bids, 'hb', 'b');
    const b = s.book.bids[0], a = s.book.asks[0]; $('#hsp').textContent = a && b ? `spread ${a[0] - b[0]} ticks · best ${b[0]} / ${a[0]}` : 'book empty';
    for (const x of s.trades || []) tapeQ.push(`<em>#${x[0]}</em> ${fmt(x[4])} @ <b class="p">${x[3]}</b>`);
    if (tapeQ.length) { const items = $$('#tape span'); const last = tapeQ.splice(0).slice(-14); last.forEach((h, i) => { items[i].innerHTML = h; items[i + 14].innerHTML = h; }); }
    if (!started && !s.sim) { started = true; api('/api/sim', { action: 'start', workload: 'mixed', rate: 50000 }).catch(() => {}); } // idle engine: start the demo so the page is alive
  }

  function loop() {
    for (const el of $$('[data-n]')) { const k = el.dataset.n, t = tgt[k]; if (t === undefined) continue; cur[k] = (cur[k] ?? 0) + (t - (cur[k] ?? 0)) * 0.14; if (Math.abs(t - cur[k]) < 0.6) cur[k] = t;
      el.textContent = el.dataset.f === 'ns' ? ns(cur[k]) : fmt(Math.round(cur[k])); }
    const y = scrollY, h = document.documentElement.scrollHeight - innerHeight;
    $('#lp').style.transform = `scaleX(${h > 0 ? y / h : 0})`;
    if (!reduced) { const hb = $('#hb'); if (hb && y < innerHeight) hb.style.transform = `translateY(${y * -0.06}px)`; }
    raf = requestAnimationFrame(loop);
  }

  window.Landing = {
    enter() { if (!built) build(); scrollTo(0, 0); cancelAnimationFrame(raf); loop(); if (typeof S !== 'undefined' && S) snap(S); },
    leave() { cancelAnimationFrame(raf); },
    snap,
  };
})();
