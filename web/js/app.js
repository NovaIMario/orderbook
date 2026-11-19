import { Simulation, AGENT_TYPES, DEFAULTS } from './sim.js';

const $ = (s) => document.querySelector(s);
const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const usd = (ticks) => (ticks == null ? '–' : `$${(ticks / 100).toFixed(2)}`);
const money = (v) => `${v < 0 ? '−' : '+'}$${Math.abs(v).toLocaleString('en-GB', { maximumFractionDigits: 0 })}`;
const int = (v) => v.toLocaleString('en-GB');

const sim = new Simulation({}, (Math.random() * 1e9) | 0);
let running = !matchMedia('(prefers-reduced-motion: reduce)').matches;
let speed = 6;

// ---------- controls ----------
const playBtn = $('#play');
const setPlay = () => { playBtn.textContent = running ? 'Pause' : 'Play'; };
setPlay();
playBtn.addEventListener('click', () => { running = !running; setPlay(); });
$('#reset').addEventListener('click', () => { sim.seed = (Math.random() * 1e9) | 0; sim.reset(); draw(); });
$('#speed').addEventListener('input', (e) => { speed = +e.target.value; });
$('#news-up').addEventListener('click', () => sim.news(1));
$('#news-down').addEventListener('click', () => sim.news(-1));
$('#crash').addEventListener('click', () => sim.flashCrash());

for (const key of Object.keys(DEFAULTS)) {
  const el = document.getElementById(key);
  if (!el) continue;
  const out = document.getElementById(`o-${key}`);
  out.textContent = el.value;
  el.addEventListener('input', () => { out.textContent = el.value; sim.setParams({ [key]: +el.value }); });
}

const youQty = () => Math.max(1, Math.min(5000, Math.round(+$('#you-qty').value || 0)));
let lastYou = '';
function youTrade(side) {
  const before = sim.mid();
  const q = youQty();
  const r = sim.youTrade(side, q);
  if (!r.filled) { lastYou = 'Nothing to trade against: that side of the book is empty.'; return; }
  // Average fill price from the tape entries just added.
  const fills = sim.tape.filter((x) => x.taker === 'you').slice(0, 200);
  let got = 0, cost = 0;
  for (const f of fills) { if (got >= r.filled) break; const take = Math.min(f.qty, r.filled - got); got += take; cost += take * f.price; }
  const avg = cost / got;
  const impact = side === 'buy' ? avg - before : before - avg;
  lastYou = `${side === 'buy' ? 'Bought' : 'Sold'} ${int(r.filled)} at an average of ${usd(avg)}, ${(impact / 100).toFixed(2)} ${impact >= 0 ? 'worse' : 'better'} than the mid (${(impact / before * 1e4).toFixed(1)} bp market impact).`;
  draw();
}
$('#you-buy').addEventListener('click', () => youTrade('buy'));
$('#you-sell').addEventListener('click', () => youTrade('sell'));

$('#theme-toggle').addEventListener('click', () => {
  const dark = document.documentElement.dataset.theme ? document.documentElement.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.dataset.theme = dark ? 'light' : 'dark';
  try { localStorage.setItem('theme', dark ? 'light' : 'dark'); } catch (e) {}
  draw();
});

// ---------- canvas helpers ----------
function fit(canvas) {
  const r = canvas.parentElement.getBoundingClientRect();
  const dpr = devicePixelRatio || 1;
  const W = Math.max(100, r.width), H = Math.max(80, r.height);
  if (canvas.width !== Math.round(W * dpr) || canvas.height !== Math.round(H * dpr)) {
    canvas.width = Math.round(W * dpr); canvas.height = Math.round(H * dpr);
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  g.font = '11px system-ui, -apple-system, "Segoe UI", sans-serif';
  return { g, W, H };
}

function niceStep(span, count) {
  const raw = span / count, mag = 10 ** Math.floor(Math.log10(raw));
  return [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw);
}

const WINDOW = 1500;

function drawPrice() {
  const { g, W, H } = fit($('#price'));
  const h = sim.history.slice(-WINDOW);
  if (h.length < 2) return;
  const axisW = 60, top = 8, bottom = 18, plotW = W - axisW, plotH = H - top - bottom;
  let lo = Infinity, hi = -Infinity;
  for (const p of h) {
    for (const v of [p.mid, p.fair, p.bid, p.ask]) if (v != null) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
  }
  const pad = Math.max(10, (hi - lo) * 0.08);
  lo -= pad; hi += pad;
  const t0 = h[0].t;
  const x = (t) => ((t - t0) / (WINDOW - 1)) * plotW;
  const y = (v) => top + ((hi - v) / (hi - lo)) * plotH;
  const grid = css('--grid'), muted = css('--muted');
  const step = niceStep((hi - lo) / 100, 5) * 100;
  g.textBaseline = 'middle';
  for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
    g.fillStyle = grid; g.fillRect(0, Math.round(y(v)) + 0.5, plotW, 1);
    g.fillStyle = muted; g.fillText(usd(v), plotW + 8, y(v));
  }
  // Spread band.
  g.fillStyle = css('--band');
  g.beginPath();
  let started = false;
  for (const p of h) if (p.ask != null) { started ? g.lineTo(x(p.t), y(p.ask)) : g.moveTo(x(p.t), y(p.ask)); started = true; }
  for (let i = h.length - 1; i >= 0; i--) if (h[i].bid != null) g.lineTo(x(h[i].t), y(h[i].bid));
  g.fill();
  // Events.
  g.textBaseline = 'top';
  for (const e of sim.events) {
    if (e.t < t0) continue;
    const ex = Math.round(x(e.t)) + 0.5;
    g.fillStyle = muted; g.fillRect(ex, top, 1, plotH);
    g.fillText(e.label, ex + 4, top + 2);
  }
  const line = (key, color, width, dash) => {
    g.save(); g.strokeStyle = color; g.lineWidth = width; g.lineJoin = 'round';
    if (dash) g.setLineDash(dash);
    g.beginPath();
    h.forEach((p, i) => (i ? g.lineTo(x(p.t), y(p[key])) : g.moveTo(x(p.t), y(p[key]))));
    g.stroke(); g.restore();
  };
  line('fair', css('--series-2'), 1.5, [5, 4]);
  line('mid', css('--series-1'), 2);
  // Last price tag.
  const last = h[h.length - 1].mid;
  g.fillStyle = css('--series-1');
  g.fillRect(plotW + 2, y(last) - 9, axisW - 2, 18);
  g.fillStyle = '#fff'; g.textBaseline = 'middle';
  g.fillText(usd(last), plotW + 6, y(last));
}

function drawDepth() {
  const { g, W, H } = fit($('#depth'));
  const bids = sim.book.depth('buy', 400), asks = sim.book.depth('sell', 400);
  const mid = sim.mid();
  // Show the part of the book that holds most of the liquidity: out to the 30th level on either side.
  const far = Math.max(...[bids[Math.min(29, bids.length - 1)], asks[Math.min(29, asks.length - 1)]].filter(Boolean).map((l) => Math.abs(l.price - mid)), 0);
  const range = Math.min(mid * 0.05, Math.max(25, far * 1.1));
  const lo = mid - range, hi = mid + range;
  const cum = (levels, inRange) => { let s = 0; return levels.filter((l) => inRange(l.price)).map((l) => ({ price: l.price, cum: (s += l.qty) })); };
  const b = cum(bids, (p) => p >= lo), a = cum(asks, (p) => p <= hi);
  const maxQ = Math.max(1, b.at(-1)?.cum || 0, a.at(-1)?.cum || 0);
  const bottom = 18, top = 6, plotH = H - top - bottom, axisW = 60, plotW = W - axisW;
  const x = (p) => ((p - lo) / (hi - lo)) * plotW;
  const y = (q) => top + plotH - (q / maxQ) * plotH;
  const grid = css('--grid'), muted = css('--muted');
  g.textBaseline = 'middle';
  const qs = niceStep(maxQ, 3);
  for (let q = qs; q <= maxQ; q += qs) {
    g.fillStyle = grid; g.fillRect(0, Math.round(y(q)) + 0.5, plotW, 1);
    g.fillStyle = muted; g.fillText(int(q), plotW + 8, y(q));
  }
  g.fillStyle = css('--axis'); g.fillRect(0, top + plotH, plotW, 1);
  const area = (pts, edge, color) => {
    if (!pts.length) return;
    g.beginPath();
    g.moveTo(x(pts[0].price), y(0));
    let prevQ = 0;
    for (const p of pts) { g.lineTo(x(p.price), y(prevQ)); g.lineTo(x(p.price), y(p.cum)); prevQ = p.cum; }
    g.lineTo(x(edge), y(prevQ)); g.lineTo(x(edge), y(0)); g.closePath();
    g.fillStyle = color + '40'; g.fill();
    g.strokeStyle = color; g.lineWidth = 2; g.stroke();
  };
  area(b, lo, css('--up'));
  area(a, hi, css('--down'));
  g.fillStyle = muted; g.textAlign = 'center'; g.textBaseline = 'top';
  for (const p of [lo + range * 0.25, mid, hi - range * 0.25]) g.fillText(usd(p), x(p), top + plotH + 4);
  g.textAlign = 'left';
}

function drawLadder() {
  const N = 10;
  const asks = sim.book.depth('sell', N).reverse(), bids = sim.book.depth('buy', N);
  const max = Math.max(1, ...asks.map((l) => l.qty), ...bids.map((l) => l.qty));
  const row = (l, cls) => `<tr class="${cls}"><td class="px lv">${usd(l.price)}</td><td><span class="bar" style="width:${(l.qty / max) * 100}%"></span><span class="lv">${int(l.qty)}</span></td><td class="lv">${l.orders}</td></tr>`;
  const pad = (k, cls) => Array.from({ length: k }, () => `<tr class="${cls}"><td>&nbsp;</td><td></td><td></td></tr>`).join('');
  const b = sim.book.bestBid(), a = sim.book.bestAsk();
  const spread = b != null && a != null ? `Spread ${usd(a - b)} · ${(((a - b) / ((a + b) / 2)) * 1e4).toFixed(1)} bp` : 'One-sided book: no spread';
  $('#ladder').innerHTML = `<thead><tr><th>Price</th><th>Shares</th><th>Orders</th></tr></thead><tbody>
    ${pad(N - asks.length, 'ask')}${asks.map((l) => row(l, 'ask')).join('')}
    <tr class="mid"><td colspan="3">${spread}</td></tr>
    ${bids.map((l) => row(l, 'bid')).join('')}${pad(N - bids.length, 'bid')}</tbody>`;
}

const NAMES = Object.fromEntries(AGENT_TYPES.map((t) => [t.id, t.name]));
NAMES.crash = 'Sell program';
const SHORT = { mm: 'Maker', noise: 'Noise', momentum: 'Momentum', informed: 'Informed', you: 'You', crash: 'Sell program' };

function drawTape() {
  $('#tape').innerHTML = `<thead><tr><th>Price</th><th>Shares</th><th>Aggressor</th></tr></thead><tbody>` +
    sim.tape.slice(0, 14).map((t) => `<tr><td class="${t.side === 'buy' ? 'b' : 's'}">${t.side === 'buy' ? '▲' : '▼'} ${usd(t.price)}</td><td>${int(t.qty)}</td><td>${SHORT[t.taker] ?? '–'}</td></tr>`).join('') + '</tbody>';
}

function drawStats() {
  const b = sim.book.bestBid(), a = sim.book.bestAsk();
  const stat = (label, val) => `<div class="stat"><span>${label}</span><b>${val}</b></div>`;
  const gap = (sim.last / sim.fair - 1) * 100;
  $('#stats').innerHTML =
    stat('Last trade', usd(sim.last)) +
    stat('Bid / ask', `${usd(b)} / ${usd(a)}`) +
    stat('Fair value', `${usd(sim.fair)} <small class="${Math.abs(gap) < 0.5 ? 'muted' : gap > 0 ? 'pos' : 'neg'}">${gap >= 0 ? '+' : '−'}${Math.abs(gap).toFixed(2)}%</small>`) +
    stat('Volume', int(sim.volume)) +
    stat('Resting orders', int(sim.book.orders.size));
}

function drawAgents() {
  const s = sim.summary();
  $('#agents').innerHTML = `<thead><tr><th>Trader type</th><th>How many</th><th>Profit / loss</th><th>Net position</th></tr></thead><tbody>` +
    AGENT_TYPES.map((t) => `<tr><td>${t.name}</td><td>${s[t.id].count}</td><td class="${s[t.id].pnl >= 0 ? 'pos' : 'neg'}">${money(s[t.id].pnl)}</td><td>${int(s[t.id].inv)}</td></tr>`).join('') + '</tbody>';
  const you = sim.you;
  $('#you-stats').innerHTML = `${lastYou ? lastYou + '<br>' : ''}Position <b>${int(you.inv)}</b> shares · P&amp;L <b class="${sim.pnl(you) >= 0 ? 'pos' : 'neg'}">${money(sim.pnl(you))}</b>`;
}

let frame = 0;
function draw() {
  drawPrice();
  drawDepth();
  drawLadder();
  drawStats();
  if (frame % 3 === 0) { drawTape(); drawAgents(); }
}

function loop() {
  if (running && !document.hidden) for (let i = 0; i < speed; i++) sim.step();
  frame++;
  if (running || frame % 15 === 0) draw();
  requestAnimationFrame(loop);
}

// Warm up so the chart starts with some history.
for (let i = 0; i < 400; i++) sim.step();
draw();
requestAnimationFrame(loop);
new ResizeObserver(() => draw()).observe(document.body);
