// JavaScript port of the C++ OrderBook's matching rules (price-time priority, trades at the
// maker's price, market/IOC orders never rest), kept simple for the browser simulation: levels
// are queues in a Map plus a sorted price list per side. Each order carries the id of the agent
// that sent it so the simulation can settle fills.

export class Book {
  constructor() { this.reset(); }

  reset() {
    this.levels = { buy: new Map(), sell: new Map() };
    this.prices = { buy: [], sell: [] }; // buy: high -> low, sell: low -> high
    this.orders = new Map();
    this.nextId = 1;
    this.trades = [];
  }

  bestBid() { return this.prices.buy.length ? this.prices.buy[0] : null; }
  bestAsk() { return this.prices.sell.length ? this.prices.sell[0] : null; }

  // Insert a price into a side's sorted list (binary search).
  addPrice(side, p) {
    const arr = this.prices[side];
    let lo = 0, hi = arr.length;
    while (lo < hi) {
      const m = (lo + hi) >> 1;
      if (side === 'buy' ? arr[m] > p : arr[m] < p) lo = m + 1; else hi = m;
    }
    arr.splice(lo, 0, p);
  }
  removePrice(side, p) {
    const arr = this.prices[side];
    const i = arr.indexOf(p);
    if (i >= 0) arr.splice(i, 1);
  }

  match(side, limit, qty, taker, owner) {
    const opp = side === 'buy' ? 'sell' : 'buy';
    while (qty > 0 && this.prices[opp].length) {
      const p = this.prices[opp][0];
      if (side === 'buy' ? p > limit : p < limit) break;
      const lvl = this.levels[opp].get(p);
      while (qty > 0 && lvl.queue.length) {
        const m = lvl.queue[0];
        const fill = Math.min(qty, m.qty);
        this.trades.push({ maker: m.id, taker, makerOwner: m.owner, takerOwner: owner, price: p, qty: fill, side });
        qty -= fill; m.qty -= fill; lvl.qty -= fill;
        if (m.qty === 0) { lvl.queue.shift(); this.orders.delete(m.id); }
      }
      if (!lvl.queue.length) { this.levels[opp].delete(p); this.prices[opp].shift(); }
    }
    return qty;
  }

  // Returns { id, filled, resting }. Trades for this call are appended to this.trades.
  limit(side, price, qty, owner = 0, tif = 'GTC') {
    if (!(qty > 0) || !(price > 0)) return { id: 0, filled: 0, resting: 0 };
    const id = this.nextId++;
    const left = this.match(side, price, qty, id, owner);
    if (left > 0 && tif === 'GTC') {
      let lvl = this.levels[side].get(price);
      if (!lvl) { lvl = { qty: 0, queue: [] }; this.levels[side].set(price, lvl); this.addPrice(side, price); }
      const o = { id, side, price, qty: left, owner };
      lvl.queue.push(o); lvl.qty += left;
      this.orders.set(id, o);
    }
    return { id, filled: qty - left, resting: tif === 'GTC' ? left : 0 };
  }

  market(side, qty, owner = 0) {
    return this.limit(side, side === 'buy' ? Infinity : 1e-9, qty, owner, 'IOC');
  }

  cancel(id) {
    const o = this.orders.get(id);
    if (!o) return false;
    const lvl = this.levels[o.side].get(o.price);
    lvl.queue.splice(lvl.queue.indexOf(o), 1);
    lvl.qty -= o.qty;
    if (!lvl.queue.length) { this.levels[o.side].delete(o.price); this.removePrice(o.side, o.price); }
    this.orders.delete(id);
    return true;
  }

  depth(side, n) {
    return this.prices[side].slice(0, n).map((p) => {
      const l = this.levels[side].get(p);
      return { price: p, qty: l.qty, orders: l.queue.length };
    });
  }
}
