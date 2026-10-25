'use strict';
// 从一个导出目录做出可以公开提交的小样例（data/sample/）：
//   - 币种改名为 S01、S02…，时间整体平移到 2001 年起：看不出原始信号来自哪个频道、哪一天
//   - K 线只保留每个币种「第一张单前一根 → 该币种最后一笔平仓」这段，其余时间没有持仓，用不到
//   - 价格、数量原样保留：样例的作用是让 golden 测试在公开仓库里也能逐笔对上 JS 的结果
// 裁掉的 K 线会不会改变结果？不会盲信：tests/test_golden.cpp 会在样例上重新跑一遍逐笔对照。
//
// 用法: node tools/make_sample.js <导出目录> <输出目录>
const fs = require('fs');
const path = require('path');

const [SRC, DST] = process.argv.slice(2);
if (!SRC || !DST) {
  console.error('用法: node tools/make_sample.js <导出目录> <输出目录>');
  process.exit(1);
}

const readCsv = (file) => {
  const [header, ...lines] = fs.readFileSync(path.join(SRC, file), 'utf8').trim().split('\n');
  const cols = header.split(',');
  return { cols, rows: lines.map((l) => l.split(',')) };
};
const writeCsv = (file, { cols, rows }) =>
  fs.writeFileSync(path.join(DST, file), [cols.join(','), ...rows.map((r) => r.join(','))].join('\n') + '\n');

const orders = readCsv('orders.csv');
const events = readCsv('events.csv');
const golden = readCsv('golden.csv');
const summary = readCsv('summary.csv');
const col = (t, name) => t.cols.indexOf(name);

// 时间平移量：按天取整，K 线网格（分钟）对齐不受影响
const DAY = 24 * 3600 * 1000;
const firstT = Math.min(...orders.rows.map((r) => Number(r[col(orders, 't_ms')])));
const offset = Math.floor(firstT / DAY) * DAY - Date.UTC(2001, 0, 1);
const shift = (v) => (v === '' ? v : String(Number(v) - offset));

// 币种改名（按第一次出现的顺序编号）
const rename = new Map();
for (const r of orders.rows) {
  const sym = r[col(orders, 'symbol')];
  if (!rename.has(sym)) rename.set(sym, 'S' + String(rename.size + 1).padStart(2, '0'));
}

// 每个币种需要的 K 线区间
const symbolOf = new Map(orders.rows.map((r) => [r[col(orders, 'order_id')], r[col(orders, 'symbol')]]));
const windowOf = new Map();
const widen = (sym, t) => {
  const w = windowOf.get(sym) || { from: Infinity, to: -Infinity };
  w.from = Math.min(w.from, t);
  w.to = Math.max(w.to, t);
  windowOf.set(sym, w);
};
for (const r of orders.rows) widen(r[col(orders, 'symbol')], Number(r[col(orders, 't_ms')]));
for (const r of golden.rows) widen(symbolOf.get(r[col(golden, 'order_id')]), Number(r[col(golden, 't_ms')]));
for (const r of events.rows) {
  if (r[col(events, 'a')] === 'backtest_end_unfilled') continue;   // 撤掉从没成交的挂单：不需要 K 线
  widen(symbolOf.get(r[col(events, 'order_id')]), Number(r[col(events, 't_ms')]));
}

fs.mkdirSync(path.join(DST, 'candles'), { recursive: true });
const gran = Number(summary.rows[0][col(summary, 'gran_ms')]);
let candleRows = 0;
for (const [sym, w] of windowOf) {
  const file = path.join(SRC, 'candles', `${sym}.csv`);
  if (!fs.existsSync(file)) continue;
  const [header, ...lines] = fs.readFileSync(file, 'utf8').trim().split('\n');
  const kept = lines.filter((l) => {
    const ts = Number(l.slice(0, l.indexOf(',')));
    return ts >= w.from - gran && ts <= w.to + gran;
  }).map((l) => {
    const i = l.indexOf(',');
    return shift(l.slice(0, i)) + l.slice(i);
  });
  candleRows += kept.length;
  fs.writeFileSync(path.join(DST, 'candles', `${rename.get(sym)}.csv`), [header, ...kept].join('\n') + '\n');
}

for (const r of orders.rows) {
  r[col(orders, 't_ms')] = shift(r[col(orders, 't_ms')]);
  r[col(orders, 'symbol')] = rename.get(r[col(orders, 'symbol')]);
}
for (const r of events.rows) r[col(events, 't_ms')] = shift(r[col(events, 't_ms')]);
for (const r of golden.rows) r[col(golden, 't_ms')] = shift(r[col(golden, 't_ms')]);
const s = summary.rows[0];
s[col(summary, 'last_message_ms')] = shift(s[col(summary, 'last_message_ms')]);
s[col(summary, 'frozen_now_ms')] = shift(s[col(summary, 'frozen_now_ms')]);

writeCsv('orders.csv', orders);
writeCsv('events.csv', events);
writeCsv('golden.csv', golden);
writeCsv('summary.csv', summary);
fs.copyFileSync(path.join(SRC, 'channel.csv'), path.join(DST, 'channel.csv'));
console.log(`[样例] ${orders.rows.length} 笔订单, ${rename.size} 个币种, ${candleRows} 根 K 线 → ${DST}`);
