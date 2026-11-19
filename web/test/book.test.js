import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Book } from '../js/book.js';
import { Simulation } from '../js/sim.js';

test('price-time priority and maker price', () => {
  const b = new Book();
  const a = b.limit('sell', 101, 5);
  const c = b.limit('sell', 100, 5);
  const d = b.limit('sell', 100, 5);
  const t = b.limit('buy', 101, 12);
  assert.equal(t.filled, 12);
  assert.deepEqual(b.trades.map((x) => [x.maker, x.price, x.qty]), [[c.id, 100, 5], [d.id, 100, 5], [a.id, 101, 2]]);
  assert.equal(b.bestAsk(), 101);
});

test('market orders never rest; cancel removes empty levels', () => {
  const b = new Book();
  b.limit('sell', 100, 3);
  const m = b.market('buy', 10);
  assert.equal(m.filled, 3);
  assert.equal(b.bestBid(), null);
  const x = b.limit('buy', 99, 4);
  assert.ok(b.cancel(x.id));
  assert.ok(!b.cancel(x.id));
  assert.equal(b.bestBid(), null);
});

test('closed market conserves shares and cash', () => {
  const s = new Simulation({}, 9);
  for (let i = 0; i < 5000; i++) s.step();
  s.news(1);
  for (let i = 0; i < 2000; i++) s.step();
  // Every trade is between two agents, so positions and cash must net to zero.
  assert.equal(s.agents.reduce((a, x) => a + x.inv, 0), 0);
  assert.equal(s.agents.reduce((a, x) => a + x.cash, 0), 0);
  const b = s.book.bestBid(), a = s.book.bestAsk();
  assert.ok(b == null || a == null || b < a, 'book must not be crossed');
  assert.ok(Math.abs(s.mid() / s.fair - 1) < 0.05, 'price should stay near fair value with the default traders');
});
