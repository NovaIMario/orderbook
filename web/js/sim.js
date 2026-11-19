// Agent-based market simulation on top of Book. Prices are integer ticks (1 tick = $0.01).
//
// Agents:
//   Market makers  quote both sides around the mid, lean their quotes against their inventory,
//                  and earn the spread. They provide almost all of the liquidity.
//   Noise traders  send small random market orders and leave random limit orders near the touch.
//   Momentum       buy after the price has risen and sell after it has fallen.
//   Informed       know the hidden fair value and trade when the market price is far from it.

import { Book } from './book.js';

export const AGENT_TYPES = [
  { id: 'mm', name: 'Market makers' },
  { id: 'noise', name: 'Noise traders' },
  { id: 'momentum', name: 'Momentum traders' },
  { id: 'informed', name: 'Informed traders' },
  { id: 'you', name: 'You' },
];

function mulberry32(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export const DEFAULTS = {
  makers: 4,
  makerSpread: 6,     // ticks each side of the reference price
  makerSize: 40,
  noise: 5,           // relative activity, 0..10
  momentum: 3,
  informed: 2,
  volatility: 4,      // fair value volatility, 0..10
};

export class Simulation {
  constructor(params = {}, seed = 1) {
    this.params = { ...DEFAULTS, ...params };
    this.seed = seed;
    this.reset();
  }

  reset() {
    this.rand = mulberry32(this.seed);
    this.book = new Book();
    this.t = 0;
    this.fair = 10000;          // $100.00
    this.last = 10000;
    this.agents = [];
    this.history = [];          // per step: { t, last, bid, ask, fair }
    this.tape = [];             // recent trades, newest first
    this.volume = 0;
    this.tradeCount = 0;
    this.flow = 0;
    this.crash = null;
    this.stress = 1;              // decaying sum of signed taker volume (buys positive)
    this.events = [];
    this.buildAgents();
    // Seed the book so the first steps have something to trade against.
    for (let k = 0; k < 6; k++) for (const a of this.agents) if (a.type === 'mm') this.requote(a);
  }

  buildAgents() {
    const p = this.params;
    const old = this.agents;
    const keep = (type, n) => {
      const have = old.filter((a) => a.type === type);
      for (let i = 0; i < n; i++) this.agents.push(have[i] || { type, cash: 0, inv: 0, orders: [], id: 0, phase: this.rand() });
      for (let i = n; i < have.length; i++) for (const o of have[i].orders) this.book.cancel(o.id);
    };
    this.agents = [];
    keep('mm', p.makers);
    keep('noise', p.noise > 0 ? 6 : 0);
    keep('momentum', p.momentum);
    keep('informed', p.informed);
    keep('you', 1);
    this.agents.forEach((a, i) => { a.id = i + 1; });
    this.byId = new Map(this.agents.map((a) => [a.id, a]));
    this.you = this.agents.find((a) => a.type === 'you');
  }

  setParams(p) {
    const recount = ['makers', 'momentum', 'informed', 'noise'].some((k) => k in p && p[k] !== this.params[k]);
    Object.assign(this.params, p);
    if (recount) {
      // Re-key resting orders to the new agent ids.
      const before = new Map(this.agents.map((a) => [a.id, a]));
      this.buildAgents();
      for (const o of this.book.orders.values()) {
        const owner = before.get(o.owner);
        o.owner = owner && this.agents.includes(owner) ? owner.id : 0;
      }
    }
  }

  gauss() {
    const u = 1 - this.rand(), v = this.rand();
    return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  }

  mid() {
    const b = this.book.bestBid(), a = this.book.bestAsk();
    if (b != null && a != null) return (a + b) / 2;
    return b ?? a ?? this.last;
  }

  // ----- order helpers that keep each agent's list of live orders -----
  place(agent, side, price, qty, life = 0) {
    price = Math.max(1, Math.round(price));
    const r = this.book.limit(side, price, Math.round(qty), agent.id);
    if (r.resting > 0) agent.orders.push({ id: r.id, side, price, born: this.t, life });
    this.settle();
    return r;
  }
  hit(agent, side, qty) {
    const r = this.book.market(side, Math.round(qty), agent.id);
    this.settle();
    return r;
  }
  cancelAll(agent) {
    for (const o of agent.orders) this.book.cancel(o.id);
    agent.orders = [];
  }

  // Apply new trades to both sides' cash and inventory, and record them.
  settle() {
    const tr = this.book.trades;
    if (!tr.length) return;
    for (const x of tr) {
      const maker = this.byId.get(x.makerOwner), taker = this.byId.get(x.takerOwner);
      const sgn = x.side === 'buy' ? 1 : -1; // taker's direction
      if (taker) { taker.inv += sgn * x.qty; taker.cash -= sgn * x.qty * x.price; }
      if (maker) { maker.inv -= sgn * x.qty; maker.cash += sgn * x.qty * x.price; }
      this.last = x.price;
      this.flow += sgn * x.qty;
      this.volume += x.qty;
      this.tradeCount++;
      this.tape.unshift({ t: this.t, price: x.price, qty: x.qty, side: x.side, taker: taker?.type ?? (x.takerOwner === -1 ? 'crash' : undefined), maker: maker?.type });
    }
    if (this.tape.length > 60) this.tape.length = 60;
    tr.length = 0;
    // Drop filled orders from agents' lists.
    for (const a of this.agents) if (a.orders.length) a.orders = a.orders.filter((o) => this.book.orders.has(o.id));
  }

  requote(a) {
    const p = this.params;
    this.cancelAll(a);
    // Makers learn from order flow: persistent buying suggests someone knows the price should be
    // higher, so they move their quotes up (and vice versa).
    const lean = Math.max(-p.makerSpread, Math.min(p.makerSpread, this.flow * 0.015));
    const ref = this.mid() + lean;
    const maxInv = p.makerSize * 8;
    // Lean against inventory: a long maker quotes lower to sell more and buy less.
    const skew = (a.inv / maxInv) * p.makerSpread * 1.5;
    const half = p.makerSpread * (0.8 + 0.4 * a.phase) * (this.stress || 1);
    const bid = Math.floor(ref - half - skew), ask = Math.ceil(ref + half - skew);
    if (a.inv < maxInv) { this.place(a, 'buy', bid, p.makerSize); this.place(a, 'buy', bid - p.makerSpread, p.makerSize * 2); }
    if (a.inv > -maxInv) { this.place(a, 'sell', Math.max(ask, bid + 1), p.makerSize); this.place(a, 'sell', Math.max(ask, bid + 1) + p.makerSpread, p.makerSize * 2); }
  }

  step() {
    const p = this.params, R = () => this.rand();
    this.t++;
    // Hidden fair value: a random walk.
    this.fair *= Math.exp(this.gauss() * 0.00005 * p.volatility);
    this.flow *= 0.95;
    if (this.crash) {
      const q = Math.min(this.crash.left, 60);
      this.book.market('sell', q, -1);
      this.settle();
      this.crash.left -= q;
      if (this.crash.left <= 0) this.crash = null;
    }
    // Makers widen their quotes when the price has been jumping around (they fear being picked off).
    const h = this.history;
    const jump = h.length > 20 ? Math.abs(this.last - h[h.length - 20].last) : 0;
    this.stress = Math.min(4, Math.max(1, jump / (3 * p.makerSpread)));

    for (const a of this.agents) {
      switch (a.type) {
        case 'mm':
          if (R() < 0.25) this.requote(a);
          break;
        case 'noise':
          if (R() < 0.02 * p.noise) this.hit(a, R() < 0.5 ? 'buy' : 'sell', 1 + Math.floor(R() * 25));
          if (R() < 0.008 * p.noise) {
            const side = R() < 0.5 ? 'buy' : 'sell';
            const off = 2 + Math.floor(-Math.log(1 - R()) * 8);
            const m = this.mid();
            this.place(a, side, side === 'buy' ? m - off : m + off, 5 + Math.floor(R() * 30), 50 + Math.floor(R() * 300));
          }
          // Old noise orders expire.
          for (const o of a.orders) if (o.life && this.t - o.born > o.life) this.book.cancel(o.id);
          a.orders = a.orders.filter((o) => this.book.orders.has(o.id));
          break;
        case 'momentum': {
          if (R() > 0.03) break;
          const h = this.history, look = 150;
          if (h.length <= look) break;
          const ret = h[h.length - 1].last / h[h.length - 1 - look].last - 1;
          if (Math.abs(ret) > 0.004) this.hit(a, ret > 0 ? 'buy' : 'sell', 10 + Math.floor(R() * 20));
          break;
        }
        case 'informed': {
          if (R() > 0.08) break;
          const b = this.book.bestBid(), s = this.book.bestAsk();
          // Trade harder the further the price is from fair value.
          if (s != null && this.fair > s * 1.001) this.hit(a, 'buy', Math.min(150, 15 + Math.round((this.fair / s - 1) * 4000 * R())));
          else if (b != null && this.fair < b * 0.999) this.hit(a, 'sell', Math.min(150, 15 + Math.round((1 - this.fair / b) * 4000 * R())));
          break;
        }
      }
    }
    this.history.push({ t: this.t, last: this.last, mid: this.mid(), bid: this.book.bestBid(), ask: this.book.bestAsk(), fair: this.fair });
    if (this.history.length > 4000) this.history.splice(0, this.history.length - 4000);
  }

  // ----- user actions -----
  news(direction) {
    this.fair *= direction > 0 ? 1.02 : 0.98;
    this.events.push({ t: this.t, label: direction > 0 ? 'Good news' : 'Bad news' });
  }
  flashCrash() {
    // A sell program that dumps a large position as fast as it can, whatever the price, like the
    // algorithm behind the May 2010 flash crash. It sells about 12 times the visible bid depth.
    const depth = this.book.depth('buy', 50).reduce((s, l) => s + l.qty, 0);
    this.crash = { left: Math.max(3000, depth * 12) };
    this.events.push({ t: this.t, label: 'Flash crash' });
  }
  youTrade(side, qty, kind = 'market', price = null) {
    if (kind === 'market') return this.hit(this.you, side, qty);
    return this.place(this.you, side, price, qty);
  }

  // Mark-to-market profit and loss in dollars.
  pnl(a) { return (a.cash + a.inv * this.last) / 100; }

  summary() {
    const out = {};
    for (const t of AGENT_TYPES) out[t.id] = { count: 0, pnl: 0, inv: 0 };
    for (const a of this.agents) {
      const s = out[a.type];
      s.count++; s.pnl += this.pnl(a); s.inv += a.inv;
    }
    return out;
  }
}
