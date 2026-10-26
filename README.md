# p1-backtest

![CI](https://github.com/Harrison-Thorne/p1-backtest/actions/workflows/ci.yml/badge.svg)

A C++17 port of the order-matching core of a Node.js crypto trading bot's backtester, checked
trade by trade against the original, plus a parallel parameter sweep built on the work-stealing
thread pool from [p0-concurrency](https://github.com/Harrison-Thorne/p0-concurrency) (git submodule).
Standard library + GoogleTest only.

The JS bot reads trading signals from chat channels, parses them, and trades Bitget futures. Its
backtest harness replays a channel's history over 1-minute candles, but one run takes 35 s to
11 min, so trying different position sizes or take-profit splits was not practical. This repo
exports what the JS harness decided, replays the order matching in C++ and checks that it gets
the same trades, then sweeps 480 parameter sets per channel.

## Key numbers

All measured on one laptop: i7-12700H (6 P-cores + 8 E-cores, 20 logical CPUs), WSL2 Ubuntu 24.04,
g++ 13.3 `-O3`, on AC power. Data: 10 real channels (anonymised as ch01–ch11; ch06 is unsupported,
see below), 25 to 183 orders and 0.14 M to 13.5 M candle rows each. Raw output is in
[`results/`](results/).

| what | result | source |
|---|---|---|
| Trade-by-trade check vs JS | **10 / 10** supported channels identical: every opening, every close (price, size, PnL) and final equity | `results/summary.md` |
| One backtest, engine only (data in memory) | **9 ms – 443 ms** in C++ vs **35 s – 643 s** for the JS harness, **1453x – 8449x** | `results/summary.md` |
| One backtest, end to end (C++ includes reading the CSVs) | **0.09 s – 6.0 s**, **38x – 556x** faster than JS | `results/end_to_end.csv` |
| Sweep: 4800 backtests (10 channels × 480 params) | **540 s** serial → **52.7 s** on 20 workers (**10.3x**); every result identical to serial | `results/summary.md` |
| Profile-driven engine rewrite (M3 → M4) | sweep **159.6 s → 53.4 s** (median of 3, 20 workers, **3.0x**), byte-identical output | `results/engine_versions.txt` |

The JS time is the bot's own harness from start to `process.exit` (it reads its candle cache
from disk and keeps orders in SQLite), so the end-to-end row is the fair comparison. The
engine-only row shows what the sweep pays per backtest once the data is loaded. Running the 480
parameter sets through the JS harness was not attempted: the ten JS runs add up to 2302 s, so it
would be roughly 307 hours serially (arithmetic, not measured).

## How it works

```
 bot repo (Node.js, unchanged)            this repo
 ┌──────────────────────────────┐   tools/export_from_bot.js    ┌───────────────────────────┐
 │ run.js: messages → parser →  │ ─ wraps DB functions, records ─▶ orders.csv  events.csv    │
 │ channel rules → executeOrder │   every order / event / close │ golden.csv  candles/*.csv │
 │ monitor.js: fills, SL, TP    │                               └─────────────┬─────────────┘
 └──────────────────────────────┘                                             │ load_dataset
                                                                              ▼
                               ┌──────────── src/engine.cpp ───────────────────────────────┐
                               │ per 1m candle: limit fills → stop-loss → take-profit       │
                               │ decision-layer events replayed at their original sequence  │
                               └───────┬───────────────────────────────┬────────────────────┘
                     src/golden.cpp    │                               │  src/sweep.cpp
          compare with golden.csv ◀────┘                               └──▶ 480 Params × channels
          (JS config, must be equal)                                        on WorkStealingPool
```

The split matters. The bot has two layers:

- **Decision layer**: parsing signals, channel-specific rules, the author saying "close half now",
  risk checks. This is where most of the JS code is and it needs the parsed-message cache.
  It is **not** re-run in C++. Its effects are exported as an ordered event stream
  (`close`, `updateOrderTp2Hit`, `absorbDcaIntoOrder`, … 12 kinds) and replayed.
- **Matching layer**: on every candle, fill limits, check stop-loss, walk take-profit stages,
  move to break-even, DCA merges. This is what parameters like position size or TP split change,
  and it is what the engine ports, function by function (each one names its JS counterpart).

The exporter runs the bot's unmodified harness with the clock frozen, all API keys blanked, and
trading forced to paper mode. It replaces a few exported functions before the bot's modules
destructure them, and writes what happened with a global sequence number so C++ can interleave
openings, events and candles in the same order.

## Getting the same trades as JS

`golden_check` runs the engine with the JS config and compares openings (available balance,
margin, size), then every close in order (reason, price, size, PnL), then final equity, with a
relative tolerance of 1e-9. Getting there forced the port to copy JS behaviour exactly:

- **`toFixed` rounding.** JS rounds the exact decimal expansion half away from zero; `printf`
  rounds half to even. `round_fixed` prints 30 extra digits and rounds by hand (`src/engine.cpp:15`).
- **Operation order.** `0.43 × 40 / 100` and `0.43 × (40 / 100)` differ in the last bit; after
  flooring to whole lots one gives 0.171, the other 0.172. The port computes the ratio first, as
  JS does (`src/engine.cpp:390`).
- **Same-candle order.** Stop-loss is checked before take-profit, a new order is not checked
  against the candle it was opened on, and one candle can walk through several TP stages (JS
  loops up to 6 times and compares only TP1–TP5).
- **Quirks kept on purpose.** If contract info was missing, JS never takes profit on that order;
  a filled limit with a same-side open position merges into it and its margin is released;
  a `trade` event with no price adds no PnL. These are bugs or oddities of the bot, but a port
  that "fixed" them would not match, and then the check proves nothing.

Unit tests (`tests/test_engine.cpp`, 18 tests) pin each rule on hand-made candles.
`tests/test_golden.cpp` runs the same check on two committed sample datasets and also checks that
a deliberately wrong rule is caught.

One channel (ch06) uses a channel mode (`belovyMode`) where the decision layer rewrites orders in
ways that are not exported. The engine refuses it with an error instead of producing numbers that
look right.

## Sample data

Real exports contain channel signals and stay local (`data/exports/` is git-ignored).
`data/sample/channel_a` and `channel_b` (1 MB and 1.9 MB) were made by re-running the JS harness
on a short window of two channels (`EXPORT_MESSAGES_UNTIL`), then `tools/make_sample.js` renamed
symbols to `S01…`, shifted all timestamps to start on 2001-01-01 and trimmed candles to each
symbol's active window. They keep their own JS answers, so CI runs the golden check on them.

## Parameter sweep

`default_grid()` (`src/sweep.cpp:11`) is 5 position sizes × 4 leverage caps × 4 TP splits ×
trailing stop on/off × 3 break-even buffers = 480 sets. Each (channel, params) pair is one task on
p0's `WorkStealingPool`:

- Datasets are loaded once and shared as `const`, so tasks never lock anything.
- Futures are collected by index, so output order does not depend on which thread finishes
  first, and `bench_sweep` checks every parallel result equals the serial one.
- An exception inside a task comes back through `future::get()`; the pool is declared after the
  output vector so it is joined before that vector is destroyed (`src/sweep.cpp:76`).

Scaling (4800 backtests, `results/summary.md`, median of 5; 6–16 workers are single runs in
`results/sweep_extra_workers.csv`):

| workers | 1 | 2 | 4 | 6 | 8 | 12 | 14 | 16 | 20 | 40 |
|---|---|---|---|---|---|---|---|---|---|---|
| time (s) | 535.6 | 271.2 | 135.2 | 94.3 | 83.7 | 68.0 | 62.4 | 57.6 | 52.7 | 52.6 |
| speedup | 1.01x | 1.99x | 4.00x | 5.73x | 6.46x | 7.95x | 8.67x | 9.38x | 10.26x | 10.28x |

Linear up to 4–6 workers, then the curve bends. This CPU has 6 P-cores with two hyperthreads
each and 8 slower E-cores, and the sweep process holds about 2.4 GB of data in memory, so SMT, E-cores and
memory bandwidth are all candidates. I have not separated them. More workers than logical CPUs
gives nothing, as expected.

What the sweep found is in `results/sweep.csv`. The best position size differs by channel: 2 % on
five channels, 3 % on one, and 6 % on four. 2 % and 6 % are the ends of the grid, so the grid should
be wider. These are **in-sample** results on one history per channel. They show the tool works,
not that the parameters would do better live.

## Profiling

The first engine (M3) took 160 s for the sweep. `callgrind` on single backtests showed string
compares (`memcmp`) and `malloc`/`free` near the top, next to CSV parsing. The changes in
M4, all inside `src/engine.cpp`:

1. **Symbol ids.** Symbols become ints when an order opens; the candle loop compares ints.
2. **Open-order list.** Each candle scans only the indices of open orders, not every order ever
   opened (closed ones are compacted out between candles, so indices stay valid mid-candle).
3. **Reused buffers.** Per-candle scratch vectors are members, not locals.
4. **Idle-gap skip.** With no open orders nothing can change, so the loop jumps to the candle of
   the next message instead of stepping through months of empty minutes.

Result: 159.6 s → 53.4 s for the same 4800 backtests, with byte-identical sweep output
(`scripts/compare_engine_versions.sh` builds both commits from git and `cmp`s the CSVs).
After this, end-to-end time is mostly CSV loading. For example, ch05 takes 1.93 s end to end, and
51 ms of that is the backtest.

## Limits

- The decision layer is replayed, not re-run. Parameters that would change *which* signals are
  taken (filters, channel rules) cannot be swept here. When a parameter changes position size,
  sizes in decision-layer events are scaled by the same ratio.
- The fill model is the bot's: fills at the limit / stop / TP price on 1-minute OHLC, no fees,
  no slippage, no funding. The port is faithful to it, not more realistic than it.
- One machine, one OS (WSL2). Numbers on other hardware will differ.

## Correctness checks

- 25 GoogleTest tests: engine rules, golden check on both samples, sweep determinism and
  exception propagation.
- `scripts/check.sh`: the whole suite under ThreadSanitizer, then AddressSanitizer + UBSan.
- CI (GitHub Actions): release, tsan and asan-ubsan jobs on every push.

## Build and run

Needs a C++17 compiler (tested with g++ 13), CMake ≥ 3.16 and `libgtest-dev`.

```bash
git clone --recurse-submodules https://github.com/Harrison-Thorne/p1-backtest.git
cd p1-backtest
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
./build/golden_check data/sample/channel_a data/sample/channel_b
./build/sweep --out sweep.csv data/sample/channel_a data/sample/channel_b
bash scripts/check.sh                              # TSan, then ASan + UBSan
```

With exports from the bot (`node tools/export_from_bot.js <bot dir> <channelId> data/exports/<name>`):

```bash
bash scripts/run_bench.sh data/exports             # golden check, JS vs C++, scaling, sweep → results/
bash scripts/compare_engine_versions.sh data/exports/<dir>...   # M3 vs M4 engine
```

## Layout

```
include/bt/   dataset.h  engine.h  golden.h  sweep.h
src/          dataset.cpp (CSV loading)  engine.cpp (matching)  golden.cpp  sweep.cpp
apps/         golden_check.cpp  sweep.cpp
bench/        bench_sweep.cpp
tests/        test_engine.cpp  test_golden.cpp  test_sweep.cpp
tools/        export_from_bot.js  make_sample.js
scripts/      check.sh  run_bench.sh  compare_engine_versions.sh
data/sample/  two anonymised datasets with their JS answers
results/      raw benchmark output
third_party/p0-concurrency   (submodule: WorkStealingPool)
```

## 中文说明

把一个 Node.js 加密货币交易 bot 回测里的**撮合层**移植成 C++17，并逐笔和原版对照；再用
[p0-concurrency](https://github.com/Harrison-Thorne/p0-concurrency) 里的偷任务线程池并行扫参数。只用标准库和 GoogleTest。

- **为什么做**：JS 版回测一次 35 秒到 11 分钟，想试不同仓位、止盈分配根本跑不动。
- **怎么做**：bot 的「决策层」（解析信号、频道规则、博主喊单）不重写，用导出脚本记录成按序号排好的事件流；
  C++ 只重写「撮合层」（每根 1 分钟 K 线上的限价成交、止损、分档止盈、保本、DCA 合并），并原样重放事件。
- **对得上吗**：10 个支持的真实频道，每一笔开单、每一笔平仓（价格、数量、盈亏）和最终净值都与 JS 一致。
  为此要照抄 JS 的 `toFixed` 舍入、浮点运算顺序、同一根 K 线里「先止损后止盈」，连 bot 自己的怪行为也保留。
  不支持的频道模式直接报错，不给看似合理的错数字。
- **多快**（同一台 i7-12700H 笔记本，插电）：
  - 单次回测只算引擎：9–443 ms，JS 要 35–643 s。
  - 算上 C++ 读 CSV 的端到端时间：0.09–6.0 s，比 JS 快 38–556 倍。
  - 4800 次回测：串行 540 s，20 个 worker 52.7 s，加速 10.3 倍，结果和串行逐个相同。
- **性能优化**：用 callgrind 看到热点里有字符串比较和内存申请。改成币种编号、只扫未平订单、复用缓冲、空仓期直接跳过后，
  扫描从 159.6 s 降到 53.4 s，输出逐字节不变。
- **局限**：决策层只重放不重算；成交模型沿用 bot 的（不含手续费、滑点、资金费率）；扫描结果是样本内的，不代表实盘会更好。

真实导出数据含频道信号，只放本地；仓库里的两份样例已经改名、平移时间、裁剪过，并带着 JS 的对照答案。

## License

MIT
