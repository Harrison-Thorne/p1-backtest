'use strict';
// 从 JS 交易 bot 的回测 harness 导出 C++ 回测引擎需要的数据。
//
// 做法：原样运行 bot 自带的 src/backtest/run.js（AI 解析走它的本地缓存，不调 Gemini），
// 在它的数据库函数外面包一层「记录器」，把发生过的事情按时间写成 CSV：
//   orders.csv   每一笔开出的订单（下单那一刻的全部参数，含算仓位用的余额与合约规格）
//   events.csv   来自「决策层」的改单事件（博主喊撤单 / 喊离场 / 改止盈…），C++ 原样重放
//   golden.csv   JS 撮合层（K 线推进）产生的每一笔平仓 → C++ 引擎的对照答案
//   summary.csv  JS 跑出来的最终净值等
//   candles/*.csv 涉及到的币种的 K 线（只截取用得到的时间段）
//
// 用法: node tools/export_from_bot.js <bot 目录> <channelId> <输出目录> [freezeIso] [initialBalance=1000]
//   freezeIso：把 Date.now() 冻结在这个时刻（harness 用它决定回放到哪天为止）。
//              默认取该频道上次回测的生成时间。
// 环境变量 EXPORT_MESSAGES_UNTIL=<ISO 时间>：只回放这之前的消息，并在这之后 1 天收尾。
//   用来做小样例（tools/make_sample.js）：JS 对这一小段重新跑一遍，得到一份完整、自洽的对照答案
const path = require('path');
const fs   = require('fs');

const [BOT_DIR, CHANNEL_ID, OUT_DIR, FREEZE_ARG, INIT_ARG] = process.argv.slice(2);
if (!BOT_DIR || !CHANNEL_ID || !OUT_DIR) {
  console.error('用法: node tools/export_from_bot.js <bot 目录> <channelId> <输出目录> [freezeIso] [initialBalance]');
  process.exit(1);
}
const bot = (p) => path.resolve(BOT_DIR, p);
const INIT_BAL = INIT_ARG || '1000';
const UNTIL_MS = process.env.EXPORT_MESSAGES_UNTIL ? new Date(process.env.EXPORT_MESSAGES_UNTIL).getTime() : null;
const LABEL = UNTIL_MS ? 'p1sample' : 'p1export';

// ── 1. 冻结时钟 ───────────────────────────────────────────────────────────────
// 默认用该频道上次回测生成的时刻（有 base 变体用 base，否则取最晚的一次）：那时的 K 线缓存正好覆盖到这里
function lastBacktestTime() {
  const dir = bot(`data/backtest/${CHANNEL_ID}`);
  const snaps = fs.readdirSync(dir)
    .filter((d) => d !== LABEL)   // 跳过本脚本自己上次写的快照，它的时间是导出那一刻
    .map((d) => ({ variant: d, file: path.join(dir, d, 'config-snapshot.json') }))
    .filter((s) => fs.existsSync(s.file))
    .map((s) => ({ ...s, at: JSON.parse(fs.readFileSync(s.file, 'utf8')).generatedAt }));
  if (!snaps.length) throw new Error(`${dir} 下没有任何 config-snapshot.json，请手动给出 freezeIso`);
  const base = snaps.find((s) => s.variant === 'base');
  return base ? base.at : snaps.map((s) => s.at).sort().pop();
}
const FROZEN_MS = FREEZE_ARG ? new Date(FREEZE_ARG).getTime()
                : UNTIL_MS ? UNTIL_MS + 24 * 3600 * 1000
                : new Date(lastBacktestTime()).getTime();

// 只回放一段消息：harness 用 fs.readFileSync 读 messages.json，在这里把超出时间的消息滤掉
if (UNTIL_MS) {
  const msgFile = bot(`data/backtest/${CHANNEL_ID}/messages.json`);
  const origRead = fs.readFileSync;
  fs.readFileSync = function (file, ...rest) {
    const out = origRead.call(fs, file, ...rest);
    if (path.resolve(String(file)) !== msgFile) return out;
    const kept = JSON.parse(out).filter((m) => new Date(m.timestamp).getTime() < UNTIL_MS);
    return JSON.stringify(kept);
  };
}
Date.now = () => FROZEN_MS;

// ── 2. 隔离环境：不发真单、不调 Gemini / Discord / Telegram（这些变量设成空，dotenv 就不会再覆盖）
for (const k of ['GEMINI_API_KEY', 'DISCORD_TOKEN', 'TELEGRAM_BOT_TOKEN', 'BITGET_API_KEY',
                 'BITGET_SECRET_KEY', 'BITGET_PASSPHRASE']) process.env[k] = '';
process.env.USE_PROXY = 'false';
process.env.BACKTEST_GRAN = process.env.BACKTEST_GRAN || '1m';
process.env.BACKTEST_LABEL = LABEL;
process.env.PAPER_TRADING = 'true';
process.env.TRADE_ENABLED = 'false';
process.env.ARCHIVE_ENABLED = 'false';
process.env.MAX_DAILY_LOSS_PCT = '1000000';
process.env.PAPER_DB_FILE = `bot-backtest-${CHANNEL_ID}-${LABEL}.db`;
process.env.PAPER_TRADES_DIR = path.join('backtest', CHANNEL_ID, LABEL, 'trades');
process.env.BACKTEST_PARSE_CACHE = bot(`data/backtest/${CHANNEL_ID}/parse-cache`);
// 实盘里暂停的频道（disabled:true）回测时也要能跑：harness 自带的配置覆盖机制把它临时打开
process.env.BACKTEST_OVERRIDE = JSON.stringify({ disabled: false });

// ── 3. 在 bot 的模块被别人 require 之前，把要记录的函数包起来 ──────────────────
// bot 的各个模块用 const { f } = require(...) 解构导入，所以必须抢在它们加载之前替换导出
const runtime  = require(bot('src/runtime'));
const db       = require(bot('src/db/database'));
const bitget   = require(bot('src/exchange/bitget'));

const orders = [];   // 开单
const events = [];   // 决策层事件
const golden = [];   // 撮合层平仓
let inEngine = false;          // 当前是否在 replaySymbolCandle（K 线推进）里面
let lastSizing = null;         // 最近一次 executeOrder 的仓位计算输入，紧接着的 saveOrder 会用到
const contracts = {};          // symbol → 合约规格（查不到的记 null）
let lastMessageMs = 0;
let seq = 0;                   // 全局序号：同一毫秒里发生的多件事，按它还原先后顺序

const now = () => runtime.now();

const origGetContractInfo = bitget.getContractInfo;
bitget.getContractInfo = async (sym) => {
  try {
    const info = await origGetContractInfo(sym);
    contracts[sym] = info;
    return info;
  } catch (e) {
    contracts[sym] = null;
    throw e;
  }
};

// 必须在包好 getContractInfo 之后才加载 order.js：它在加载时就把 getContractInfo 解构走了
const orderMod = require(bot('src/exchange/order'));

// executeOrder：记下算仓位用的余额和入场价，C++ 换参数时要用同一公式重算仓位
const origExecute = orderMod.executeOrder;
orderMod.executeOrder = async (signal, signalId, options = {}) => {
  const available = db.getPaperAvailable();
  const res = await origExecute(signal, signalId, options);
  if (res && res.success) {
    const entry = (signal.entry && signal.entry.length) ? signal.entry : [res.entryPrice];
    lastSizing = {
      available,
      origEntry: entry.reduce((a, b) => a + b, 0) / entry.length,
      forcedLeverage: options.forceLeverage ? 1 : 0,
      positionSizePct: options.positionSizePct ?? parseFloat(process.env.POSITION_SIZE_PCT || '2'),
    };
  }
  return res;
};

const origSaveOrder = db.saveOrder;
db.saveOrder = (data) => {
  const id = Number(origSaveOrder(data));
  orders.push({ seq: ++seq, t: now(), id, data, sizing: lastSizing });
  lastSizing = null;
  return id;
};

const origSaveTradeLog = db.saveTradeLog;
db.saveTradeLog = (args) => {
  const { order, closePrice, closeSize, reason } = args;
  const rec = { seq: ++seq, t: now(), id: Number(order.id), reason, closePrice, closeSize };
  if (inEngine) golden.push(rec);
  else events.push({ kind: 'trade', ...rec });
  return origSaveTradeLog(args);
};

const origClose = db.closeOrderRecord;
db.closeOrderRecord = (id, reason, pnl) => {
  if (!inEngine) events.push({ kind: 'close', seq: ++seq, t: now(), id: Number(id), reason });
  return origClose(id, reason, pnl);
};

// 决策层其余会改订单状态的函数：记下名字和参数。C++ 只支持其中一部分，导出后统一检查
const MUTATORS = ['updateOrderTakeProfits', 'updateOrderEntryPrice', 'updateOrderRemainingSize',
  'updateOrderBreakeven', 'promoteOrderToMarket', 'setOrderBloggerMode', 'setOrderTpBaseSize',
  'updateOrderTp1Price', 'updateOrderTp2Price', 'absorbDcaIntoOrder', 'updateOrderTrailHigh',
  'updateOrderTp1Hit', 'updateOrderTp2Hit', 'updateOrderTp3Hit', 'updateOrderTp4Hit', 'updateOrderTp5Hit'];
for (const name of MUTATORS) {
  const orig = db[name];
  if (typeof orig !== 'function') continue;
  db[name] = (...args) => {
    if (!inEngine) events.push({ kind: name, seq: ++seq, t: now(), id: Number(args[0]), args: args.slice(1) });
    return orig(...args);
  };
}

// K 线只读本地缓存：harness 的缓存判断要求覆盖到「今天」，下架的币永远覆盖不全，
// 每次都会重新联网拉几十万根。导出时一律用磁盘上已有的那份，JS 和 C++ 看到的就是同一份数据
const candles = require(bot('src/backtest/candles'));
const origFetchAll = candles.CandleStore.prototype._fetchAll;
candles.CandleStore.prototype._fetchAll = async function (symbol) {
  const file = path.join(this.cacheDir, `${symbol}-${process.env.BACKTEST_GRAN}.json`);
  if (fs.existsSync(file)) {
    const cached = JSON.parse(fs.readFileSync(file, 'utf8'));
    return cached.rows || cached;
  }
  return origFetchAll.call(this, symbol);
};

const monitor = require(bot('src/position/monitor'));
const origReplay = monitor.replaySymbolCandle;
monitor.replaySymbolCandle = async (symbol, c) => {
  inEngine = true;
  try { return await origReplay(symbol, c); } finally { inEngine = false; }
};

const index = require(bot('src/index'));
const origHandle = index.handleMessage;
index.handleMessage = async (m) => {
  lastMessageMs = Math.max(lastMessageMs, new Date(m.timestamp).getTime());
  return origHandle(m);
};

// ── 4. harness 结束时会 process.exit：先把记录写出去 ───────────────────────────
const realExit = process.exit.bind(process);
// JS 回测本身用了多久（从 run.js 开始到它调用 process.exit，不含下面写 CSV）。Date.now 已被冻结，所以用 hrtime
let jsStart = 0n;
let jsSeconds = 0;
process.exit = (code) => {
  jsSeconds = Number(process.hrtime.bigint() - jsStart) / 1e9;
  if (code === 0) writeAll();
  realExit(code);
};

const num = (v) => (v == null || Number.isNaN(v) ? '' : String(v));

function writeAll() {
  fs.mkdirSync(path.join(OUT_DIR, 'candles'), { recursive: true });
  const gran = { '1m': 60_000, '5m': 300_000, '15m': 900_000, '1H': 3_600_000 }[process.env.BACKTEST_GRAN];

  // orders.csv
  const oh = ['seq', 't_ms', 'order_id', 'symbol', 'side', 'type', 'entry_price', 'limit_price', 'stop_loss',
    'leverage', 'margin', 'size', 'sl_distance_pct', 'tps', 'tp_ratios', 'orig_entry', 'available',
    'position_pct', 'forced_leverage', 'size_multiplier', 'volume_place', 'max_lever', 'contract_known'];
  const orows = orders.map(({ seq, t, id, data, sizing }) => {
    const info = contracts[data.symbol + 'USDT'];
    const tps = JSON.parse(data.take_profits || '[]');
    const ratios = data.tp_ratios ? JSON.parse(data.tp_ratios) : [];
    return [seq, t, id, data.symbol, data.action === '做多' ? 'L' : 'S', data.order_type,
      num(data.entry_price), num(data.limit_price), num(data.stop_loss), num(data.leverage),
      num(data.margin_used), num(data.size), num(data.sl_distance_pct), tps.join(';'), ratios.join(';'),
      num(sizing?.origEntry), num(sizing?.available), num(sizing?.positionSizePct),
      sizing?.forcedLeverage ?? '',
      info ? info.sizeMultiplier : 0.001, info ? info.volumePlace : 3, info ? info.maxLever : 50,
      info ? 1 : 0].join(',');
  });
  fs.writeFileSync(path.join(OUT_DIR, 'orders.csv'), [oh.join(','), ...orows].join('\n') + '\n');

  // events.csv：kind,seq,t_ms,order_id,a,b,c（含义随 kind 不同，见 C++ 的 engine.cpp）。
  // 参数里的逗号换成分号，保证每行都能按逗号直接切开
  const erows = events.map((e) => {
    if (e.kind === 'trade') return ['trade', e.seq, e.t, e.id, e.reason, num(e.closePrice), num(e.closeSize)].join(',');
    if (e.kind === 'close') return ['close', e.seq, e.t, e.id, e.reason, '', ''].join(',');
    return [e.kind, e.seq, e.t, e.id, (e.args || []).map((a) => (typeof a === 'object' ? JSON.stringify(a) : String(a))).join('|').replace(/,/g, ';'), '', ''].join(',');
  });
  fs.writeFileSync(path.join(OUT_DIR, 'events.csv'), ['kind,seq,t_ms,order_id,a,b,c', ...erows].join('\n') + '\n');

  // golden.csv
  const grows = golden.map((g) => [g.seq, g.t, g.id, g.reason, num(g.closePrice), num(g.closeSize)].join(','));
  fs.writeFileSync(path.join(OUT_DIR, 'golden.csv'), ['seq,t_ms,order_id,reason,close_price,close_size', ...grows].join('\n') + '\n');

  // summary.csv
  const s = db.getPaperStats();
  fs.writeFileSync(path.join(OUT_DIR, 'summary.csv'),
    'initial,equity,fill_count,wins,losses,gran_ms,last_message_ms,frozen_now_ms,js_seconds\n' +
    [s.initial, s.equity, s.fillCount, s.wins, s.losses, gran, lastMessageMs, FROZEN_MS, jsSeconds.toFixed(1)].join(',') + '\n');

  // 用到的 K 线：每个币种从它第一笔订单所在的 K 线开始，到回测结束
  const firstUse = {};
  for (const { t, data } of orders) firstUse[data.symbol] = Math.min(firstUse[data.symbol] ?? Infinity, t);
  const candleDir = bot('data/backtest/_candles');
  for (const [sym, t0] of Object.entries(firstUse)) {
    const file = path.join(candleDir, `${sym}-${process.env.BACKTEST_GRAN}.json`);
    if (!fs.existsSync(file)) continue;
    const cached = JSON.parse(fs.readFileSync(file, 'utf8'));
    const rows = (cached.rows || cached).filter((r) => r.ts >= Math.floor(t0 / gran) * gran - gran);
    const lines = rows.map((r) => `${r.ts},${r.open},${r.high},${r.low},${r.close}`);
    fs.writeFileSync(path.join(OUT_DIR, 'candles', `${sym}.csv`), ['ts,open,high,low,close', ...lines].join('\n') + '\n');
  }
  // channel.csv：撮合层会用到的频道开关（C++ 引擎据此决定走哪条止损 / 止盈规则）
  const cfg = require(bot('src/config/channels')).getChannelConfig(CHANNEL_ID);
  const flags = ['trailAfterTp1', 'tpLadderStop', 'tpLadderLag', 'manualTp1Tp2', 'belovyMode',
                 'entryMidToSL', 'cmpToImageLimit'];
  fs.writeFileSync(path.join(OUT_DIR, 'channel.csv'),
    'key,value\n' + flags.map((k) => `${k},${cfg[k] == null ? '' : Number(cfg[k])}`).join('\n') + '\n');

  console.log(`[导出] ${orders.length} 笔订单 / ${events.length} 个决策层事件 / ${golden.length} 笔撮合层平仓 → ${OUT_DIR}`);
}

// ── 5. 运行 bot 自己的回测（它的 main() 在 require 时就会启动）────────────────────
process.argv = [process.argv[0], bot('src/backtest/run.js'), CHANNEL_ID, INIT_BAL];
jsStart = process.hrtime.bigint();
require(bot('src/backtest/run.js'));
